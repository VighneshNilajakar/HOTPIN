"""
Hotpin Prototype - Main FastAPI Application
Real-time conversational AI server with WebSocket audio streaming

Architecture:
- FastAPI + Uvicorn ASGI server
- WebSocket for bidirectional audio streaming
- Async orchestration with sync worker thread pool offloading
- STT (Vosk) -> LLM (Groq) -> TTS (pyttsx3) pipeline

Concurrency Model:
- Async: WebSocket I/O, Groq API calls
- Sync (thread pool): Vosk transcription, pyttsx3 synthesis
"""

import os
import io
import json
import asyncio
from typing import Dict
from contextlib import asynccontextmanager
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import JSONResponse
from dotenv import load_dotenv

# Import core modules
from core.llm_client import (
    init_client, 
    close_client, 
    get_llm_response,
    clear_session_context
)
from core.stt_worker import (
    initialize_vosk_model,
    process_audio_for_transcription,
    get_model_info
)
from core.tts_worker import (
    synthesize_response_audio,
    test_tts_engine,
    get_available_voices
)

# Load environment variables
load_dotenv()

# In-memory session audio buffers
# Maps session_id -> io.BytesIO buffer containing raw PCM audio
SESSION_AUDIO_BUFFERS: Dict[str, io.BytesIO] = {}

# Server configuration
SERVER_HOST = os.getenv("SERVER_HOST", "0.0.0.0")
SERVER_PORT = int(os.getenv("SERVER_PORT", 8000))


@asynccontextmanager
async def lifespan(app: FastAPI):
    """
    Application lifespan context manager.
    Handles startup and shutdown events using modern FastAPI approach.
    """
    # Startup
    print("\n" + "="*60)
    print("🚀 Hotpin Prototype Server Starting...")
    print("="*60)
    
    # Initialize Groq LLM client
    try:
        init_client()
    except Exception as e:
        print(f"✗ Failed to initialize Groq client: {e}")
        print("⚠ Server will start but LLM functionality will not work")
    
    # Initialize Vosk STT model
    try:
        initialize_vosk_model()
        model_info = get_model_info()
        print(f"   Model: {model_info['model_path']}")
        print(f"   Format: {model_info['sample_rate']}Hz, {model_info['channels']} channel, {model_info['sample_width']*8}-bit")
    except Exception as e:
        print(f"✗ Failed to initialize Vosk model: {e}")
        print("⚠ Server will start but STT functionality will not work")
    
    # Test pyttsx3 TTS engine
    try:
        test_tts_engine()
    except Exception as e:
        print(f"✗ Failed to test TTS engine: {e}")
        print("⚠ Server will start but TTS functionality may not work")
    
    print("="*60)
    print(f"✓ Server ready at ws://{SERVER_HOST}:{SERVER_PORT}/ws")
    print("="*60 + "\n")
    
    yield  # Server runs here
    
    # Shutdown
    print("\n" + "="*60)
    print("🛑 Hotpin Prototype Server Shutting Down...")
    print("="*60)
    
    # Close Groq client
    await close_client()
    
    # Clear all session data
    SESSION_AUDIO_BUFFERS.clear()
    
    print("✓ All resources cleaned up")
    print("="*60 + "\n")


# Initialize FastAPI application with lifespan
app = FastAPI(
    title="Hotpin Conversation Prototype",
    description="Real-time voice-based conversational AI with Vosk STT, Groq LLM, and pyttsx3 TTS",
    version="1.0.0",
    lifespan=lifespan
)


@app.get("/")
async def root():
    """
    Root endpoint - API information
    """
    return JSONResponse({
        "service": "Hotpin Conversation Prototype",
        "version": "1.0.0",
        "status": "running",
        "websocket_endpoint": "/ws",
        "protocol": {
            "handshake": "Send JSON with {session_id: str}",
            "audio_input": "Stream raw PCM audio (16-bit, 16kHz, mono) as binary",
            "end_of_speech": "Send JSON with {signal: 'EOS'}",
            "audio_output": "Receive WAV audio chunks as binary"
        }
    })


@app.get("/health")
async def health_check():
    """
    Health check endpoint
    """
    model_info = get_model_info()
    return JSONResponse({
        "status": "healthy",
        "vosk_model_loaded": model_info["model_loaded"],
        "active_sessions": len(SESSION_AUDIO_BUFFERS)
    })


@app.get("/voices")
async def list_voices():
    """
    List available TTS voices
    """
    voices = get_available_voices()
    return JSONResponse({
        "voices": voices,
        "count": len(voices)
    })


@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    """
    Main WebSocket endpoint for real-time conversational AI.
    
    Protocol Flow:
    1. Client connects and sends JSON handshake with session_id
    2. Client streams binary PCM audio chunks (16-bit, 16kHz, mono)
    3. Client sends JSON with {"signal": "EOS"} to indicate end of speech
    4. Server processes: STT -> LLM -> TTS
    5. Server streams binary WAV audio response in chunks
    6. Loop continues until client disconnects
    
    Concurrency:
    - WebSocket I/O: async (non-blocking)
    - STT processing: sync in thread pool (via asyncio.to_thread)
    - LLM API call: async (non-blocking)
    - TTS synthesis: sync in thread pool (via asyncio.to_thread)
    """
    session_id = None
    
    try:
        # Accept WebSocket connection
        await websocket.accept()
        print(f"🔌 New WebSocket connection established")
        
        # Step 1: Handshake - receive session ID
        handshake_message = await websocket.receive_text()
        handshake_data = json.loads(handshake_message)
        session_id = handshake_data.get("session_id")
        
        if not session_id:
            await websocket.close(code=1008, reason="Missing session_id in handshake")
            return
        
        print(f"✓ Session initialized: {session_id}")
        
        # Initialize audio buffer for this session
        SESSION_AUDIO_BUFFERS[session_id] = io.BytesIO()
        
        # Send acknowledgment
        await websocket.send_text(json.dumps({
            "status": "connected",
            "session_id": session_id
        }))
        
        # Main communication loop
        while True:
            # Receive message (can be binary audio or text signal)
            message = await websocket.receive()
            
            # Handle binary audio data
            if "bytes" in message:
                audio_chunk = message["bytes"]
                
                # Append to session buffer
                SESSION_AUDIO_BUFFERS[session_id].write(audio_chunk)
                
                # Optional: Send progress indicator
                buffer_size = SESSION_AUDIO_BUFFERS[session_id].tell()
                if buffer_size % 32000 == 0:  # Every ~1 second at 16kHz
                    print(f"📊 [{session_id}] Buffer: {buffer_size} bytes (~{buffer_size/32000:.1f}s)")
            
            # Handle text signals (EOS, commands, etc.)
            elif "text" in message:
                signal_data = json.loads(message["text"])
                signal_type = signal_data.get("signal")
                
                if signal_type == "EOS":
                    print(f"🎤 [{session_id}] End-of-speech signal received")
                    
                    # Extract buffered PCM audio
                    pcm_data = SESSION_AUDIO_BUFFERS[session_id].getvalue()
                    
                    if len(pcm_data) == 0:
                        print(f"⚠ [{session_id}] Empty audio buffer, skipping processing")
                        # Reset buffer
                        SESSION_AUDIO_BUFFERS[session_id] = io.BytesIO()
                        continue
                    
                    print(f"🔄 [{session_id}] Processing {len(pcm_data)} bytes of audio...")
                    
                    try:
                        # Send processing indicator
                        await websocket.send_text(json.dumps({
                            "status": "processing",
                            "stage": "transcription"
                        }))
                        
                        # Step 2: STT - Transcribe audio (blocking, run in thread pool)
                        transcript = await asyncio.to_thread(
                            process_audio_for_transcription,
                            session_id,
                            pcm_data
                        )
                        
                        if not transcript or transcript.strip() == "":
                            print(f"⚠ [{session_id}] Empty transcription")
                            await websocket.send_text(json.dumps({
                                "status": "error",
                                "message": "Could not understand audio. Please try again."
                            }))
                            # Reset buffer
                            SESSION_AUDIO_BUFFERS[session_id] = io.BytesIO()
                            continue
                        
                        print(f"📝 [{session_id}] Transcript: \"{transcript}\"")
                        
                        # Send transcript to client (optional feedback)
                        await websocket.send_text(json.dumps({
                            "status": "processing",
                            "stage": "llm",
                            "transcript": transcript
                        }))
                        
                        # Step 3: LLM - Get response (async, non-blocking)
                        llm_response = await get_llm_response(session_id, transcript)
                        
                        print(f"🤖 [{session_id}] LLM response: \"{llm_response}\"")
                        
                        # Send LLM response text (optional feedback)
                        await websocket.send_text(json.dumps({
                            "status": "processing",
                            "stage": "tts",
                            "response": llm_response
                        }))
                        
                        # Step 4: TTS - Synthesize audio (blocking, run in thread pool)
                        wav_bytes = await asyncio.to_thread(
                            synthesize_response_audio,
                            llm_response
                        )
                        
                        print(f"🔊 [{session_id}] Streaming {len(wav_bytes)} bytes of audio response...")
                        
                        # Step 5: Stream audio response in chunks (async)
                        chunk_size = 4096  # 4KB chunks
                        for i in range(0, len(wav_bytes), chunk_size):
                            chunk = wav_bytes[i:i + chunk_size]
                            await websocket.send_bytes(chunk)
                        
                        # Send completion signal
                        await websocket.send_text(json.dumps({
                            "status": "complete"
                        }))
                        
                        print(f"✓ [{session_id}] Response streaming complete")
                    
                    except Exception as processing_error:
                        print(f"✗ [{session_id}] Processing error: {processing_error}")
                        await websocket.send_text(json.dumps({
                            "status": "error",
                            "message": "An error occurred while processing your request."
                        }))
                    
                    finally:
                        # Reset audio buffer for next utterance
                        SESSION_AUDIO_BUFFERS[session_id] = io.BytesIO()
                        print(f"🔄 [{session_id}] Buffer reset, ready for next input")
                
                elif signal_type == "RESET":
                    # Reset conversation context
                    clear_session_context(session_id)
                    SESSION_AUDIO_BUFFERS[session_id] = io.BytesIO()
                    await websocket.send_text(json.dumps({
                        "status": "reset_complete"
                    }))
                    print(f"🔄 [{session_id}] Session reset")
    
    except WebSocketDisconnect:
        print(f"🔌 WebSocket disconnected: {session_id}")
    
    except Exception as e:
        print(f"✗ WebSocket error [{session_id}]: {e}")
        try:
            await websocket.close(code=1011, reason=f"Server error: {str(e)}")
        except:
            pass
    
    finally:
        # Cleanup session data
        if session_id:
            if session_id in SESSION_AUDIO_BUFFERS:
                del SESSION_AUDIO_BUFFERS[session_id]
            clear_session_context(session_id)
            print(f"🧹 [{session_id}] Session cleaned up")


if __name__ == "__main__":
    import uvicorn
    
    # For development: single worker
    # For production: use multiple workers
    # Command: uvicorn main:app --host 0.0.0.0 --port 8000 --workers 4
    
    uvicorn.run(
        app,
        host=SERVER_HOST,
        port=SERVER_PORT,
        log_level="info"
    )

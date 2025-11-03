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
import socket
import subprocess
from typing import Dict
from contextlib import asynccontextmanager
from fastapi import FastAPI, WebSocket, WebSocketDisconnect, UploadFile, File, Form
from fastapi.responses import JSONResponse
from dotenv import load_dotenv

import nltk

# Download NLTK data if not already present
try:
    nltk.data.find('tokenizers/punkt')
except LookupError:
    nltk.download('punkt')
try:
    nltk.data.find('tokenizers/punkt_tab')
except LookupError:
    nltk.download('punkt_tab')

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

# Lightweight telemetry for debugging: tracks per-session audio chunks
SESSION_AUDIO_STATS: Dict[str, Dict[str, int]] = {}

# Server configuration
SERVER_HOST = os.getenv("SERVER_HOST", "0.0.0.0")
SERVER_PORT = int(os.getenv("SERVER_PORT", 8000))


def get_network_info():
    """
    Get current network information (WiFi SSID and IP address).
    Returns dict with 'wifi_name', 'ip_address', 'interface'
    """
    network_info = {
        'wifi_name': 'Unknown',
        'ip_address': 'Unknown',
        'interface': 'Unknown'
    }
    
    try:
        # Get WiFi SSID (Windows)
        if os.name == 'nt':  # Windows
            try:
                result = subprocess.run(
                    ['netsh', 'wlan', 'show', 'interfaces'],
                    capture_output=True,
                    text=True,
                    timeout=5
                )
                if result.returncode == 0:
                    for line in result.stdout.split('\n'):
                        if 'SSID' in line and 'BSSID' not in line:
                            network_info['wifi_name'] = line.split(':', 1)[1].strip()
                            network_info['interface'] = 'WiFi'
                            break
            except Exception as e:
                print(f"Could not get WiFi SSID: {e}")
        
        # Get local IP address
        try:
            # Create a socket to determine the local IP
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.settimeout(0.1)
            # Connect to a public DNS server (doesn't actually send data)
            s.connect(("8.8.8.8", 80))
            network_info['ip_address'] = s.getsockname()[0]
            s.close()
        except Exception:
            # Fallback method
            network_info['ip_address'] = socket.gethostbyname(socket.gethostname())
    
    except Exception as e:
        print(f"Error getting network info: {e}")
    
    return network_info


@asynccontextmanager
async def lifespan(app: FastAPI):
    """
    Application lifespan context manager.
    Handles startup and shutdown events using modern FastAPI approach.
    """
    # Startup
    print("\n" + "="*60)
    print("Hotpin Prototype Server Starting...")
    print("="*60)
    
    # Display network information
    network_info = get_network_info()
    print(f"\nNetwork Information:")
    print(f"   WiFi Network: {network_info['wifi_name']}")
    print(f"   IP Address: {network_info['ip_address']}")
    print(f"   Interface: {network_info['interface']}")
    print(f"   Server URL: http://{network_info['ip_address']}:{SERVER_PORT}")
    print(f"   WebSocket URL: ws://{network_info['ip_address']}:{SERVER_PORT}/ws")
    print()
    
    # Initialize Groq LLM client
    try:
        init_client()
    except Exception as e:
        print(f"Failed to initialize Groq client: {e}")
        print("⚠ Server will start but LLM functionality will not work")
    
    # Initialize Vosk STT model
    try:
        initialize_vosk_model()
        model_info = get_model_info()
        print(f"   Model: {model_info['model_path']}")
        print(f"   Format: {model_info['sample_rate']}Hz, {model_info['channels']} channel, {model_info['sample_width']*8}-bit")
    except Exception as e:
        print(f"Failed to initialize Vosk model: {e}")
        print("⚠ Server will start but STT functionality will not work")
    
    # Test pyttsx3 TTS engine
    try:
        test_tts_engine()
    except Exception as e:
        print(f"Failed to test TTS engine: {e}")
        print("⚠ Server will start but TTS functionality may not work")
    
    print("="*60)
    print(f"Server ready at ws://{SERVER_HOST}:{SERVER_PORT}/ws")
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
    
    print("All resources cleaned up")
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


@app.post("/image")
async def upload_image(
    session: str = Form(...),
    file: UploadFile = File(...)
):
    """
    Upload image from ESP32-CAM.
    
    Parameters:
    - session: Session ID (e.g., "esp32-cam-hotpin-001")
    - file: JPEG image file (multipart/form-data)
    
    Returns:
    - JSON response with success status and image metadata
    """
    try:
        # Read image data
        image_data = await file.read()
        image_size = len(image_data)
        
        print(f"📷 [{session}] Image received: {file.filename}, {image_size} bytes ({image_size/1024:.2f} KB)")
        
        # Optional: Save image to disk
        import os
        from datetime import datetime
        
        # Create images directory if it doesn't exist
        os.makedirs("captured_images", exist_ok=True)
        
        # Generate filename with timestamp
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        save_path = f"captured_images/{session}_{timestamp}.jpg"
        
        with open(save_path, "wb") as f:
            f.write(image_data)
        
        print(f"💾 [{session}] Image saved: {save_path}")
        
        # TODO: Add image processing here (e.g., object detection, OCR, etc.)
        # For now, just acknowledge receipt
        
        return JSONResponse({
            "status": "success",
            "message": "Image received successfully",
            "session": session,
            "filename": file.filename,
            "size_bytes": image_size,
            "saved_path": save_path
        })
    
    except Exception as e:
        print(f"✗ [{session}] Image upload error: {e}")
        return JSONResponse(
            status_code=500,
            content={
                "status": "error",
                "message": f"Failed to process image: {str(e)}"
            }
        )


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
        print(f"New WebSocket connection established")
        
        # Step 1: Handshake - receive session ID
        handshake_message = await websocket.receive_text()
        handshake_data = json.loads(handshake_message)
        session_id = handshake_data.get("session_id")
        
        if not session_id:
            await websocket.close(code=1008, reason="Missing session_id in handshake")
            return
        
        print(f"Session initialized: {session_id}")
        
        # Initialize audio buffer for this session
        SESSION_AUDIO_BUFFERS[session_id] = io.BytesIO()
        SESSION_AUDIO_STATS[session_id] = {"chunks": 0, "bytes": 0}
        
        # Send acknowledgment
        await websocket.send_text(json.dumps({
            "status": "connected",
            "session_id": session_id
        }))
        
        # Main communication loop
        while True:
            # Receive message (can be binary audio or text signal)
            message = await websocket.receive()

            message_type = message.get("type")
            if message_type == "websocket.disconnect":
                code = message.get("code", 1000)
                reason = message.get("reason")
                print(f"🔌 [{session_id}] WebSocket disconnect received (code={code}, reason={reason})")
                break
            
            # Handle binary audio data
            if "bytes" in message:
                audio_chunk = message["bytes"]
                
                # Append to session buffer
                SESSION_AUDIO_BUFFERS[session_id].write(audio_chunk)
                stats = SESSION_AUDIO_STATS.get(session_id)
                if stats is not None:
                    stats["chunks"] += 1
                    stats["bytes"] += len(audio_chunk)
                    if stats["chunks"] <= 5 or (stats["chunks"] % 25) == 0:
                        print(
                            f"🔊 [{session_id}] Audio chunk {stats['chunks']}: "
                            f"{len(audio_chunk)} bytes (total streamed: {stats['bytes']})"
                        )
                
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
                        SESSION_AUDIO_STATS[session_id] = {"chunks": 0, "bytes": 0}
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
                        
                        # Validate LLM response before TTS synthesis
                        if not llm_response or llm_response.strip() == "":
                            print(f"⚠ [{session_id}] Empty LLM response, using fallback message")
                            llm_response = "I'm sorry, I couldn't generate a response. Please try again."
                        
                        # Split the response into sentences
                        sentences = nltk.sent_tokenize(llm_response)

                        for sentence in sentences:
                            if not sentence.strip():
                                continue

                            # Send LLM response text (optional feedback)
                            await websocket.send_text(json.dumps({
                                "status": "processing",
                                "stage": "tts",
                                "response": sentence
                            }))
                            
                            # Step 4: TTS - Synthesize audio (blocking, run in thread pool)
                            wav_bytes = await asyncio.to_thread(
                                synthesize_response_audio,
                                sentence
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
                        import traceback
                        error_details = traceback.format_exc()
                        print(f"✗ [{session_id}] Processing error: {processing_error}")
                        print(f"   Stack trace:\n{error_details}")
                        await websocket.send_text(json.dumps({
                            "status": "error",
                            "message": "An error occurred while processing your request.",
                            "error_type": type(processing_error).__name__
                        }))
                    
                    finally:
                        # Reset audio buffer for next utterance
                        SESSION_AUDIO_BUFFERS[session_id] = io.BytesIO()
                        SESSION_AUDIO_STATS[session_id] = {"chunks": 0, "bytes": 0}
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
        print(f"WebSocket disconnected: {session_id}")
    
    except Exception as e:
        print(f"WebSocket error [{session_id}]: {e}")
        try:
            await websocket.close(code=1011, reason=f"Server error: {str(e)}")
        except:
            pass
    
    finally:
        # Cleanup session data
        if session_id:
            if session_id in SESSION_AUDIO_BUFFERS:
                del SESSION_AUDIO_BUFFERS[session_id]
            if session_id in SESSION_AUDIO_STATS:
                del SESSION_AUDIO_STATS[session_id]
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

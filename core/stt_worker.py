"""
STT Worker Module - Synchronous Vosk Speech Recognition
Handles blocking audio transcription in thread pool isolation
"""

import os
import wave
import io
import json
from vosk import Model, KaldiRecognizer
from dotenv import load_dotenv

# Load environment variables
load_dotenv()

# Global Vosk model - loaded once at startup to avoid reload overhead
VOSK_MODEL = None
VOSK_MODEL_PATH = os.getenv("VOSK_MODEL_PATH", "./model")


def initialize_vosk_model() -> None:
    """
    Initialize the global Vosk model.
    Must be called during application startup before processing any audio.
    Loads the acoustic model from the path specified in environment config.
    """
    global VOSK_MODEL
    
    if not os.path.exists(VOSK_MODEL_PATH):
        raise FileNotFoundError(
            f"Vosk model not found at: {VOSK_MODEL_PATH}\n"
            f"Please download a Vosk model and place it in the specified directory.\n"
            f"Download from: https://alphacephei.com/vosk/models"
        )
    
    print(f"Loading Vosk model from: {VOSK_MODEL_PATH}")
    VOSK_MODEL = Model(VOSK_MODEL_PATH)
    print(f"Vosk model loaded successfully")


def create_wav_header(pcm_data: bytes, sample_rate: int = 16000, 
                      channels: int = 1, sample_width: int = 2) -> bytes:
    """
    Encapsulate raw PCM audio data with proper WAV file headers.
    
    Args:
        pcm_data: Raw PCM audio bytes (16-bit signed integer)
        sample_rate: Audio sample rate in Hz (default: 16000)
        channels: Number of audio channels (default: 1 for mono)
        sample_width: Sample width in bytes (default: 2 for 16-bit)
    
    Returns:
        bytes: Complete WAV file data with headers
    
    This function uses Python's standard library `wave` module combined with
    io.BytesIO to perform in-memory conversion without disk I/O.
    WAV format: PCM with RIFF header for Vosk compatibility.
    """
    # Create in-memory byte buffer for WAV output
    wav_buffer = io.BytesIO()
    
    # Open wave file writer in memory
    with wave.open(wav_buffer, 'wb') as wav_file:
        # Set WAV parameters
        wav_file.setnchannels(channels)       # Mono audio
        wav_file.setsampwidth(sample_width)   # 16-bit = 2 bytes
        wav_file.setframerate(sample_rate)    # 16kHz sample rate
        
        # Write PCM data
        wav_file.writeframes(pcm_data)
    
    # Get complete WAV bytes from buffer
    wav_buffer.seek(0)
    wav_bytes = wav_buffer.read()
    
    return wav_bytes


def process_audio_for_transcription(session_id: str, pcm_bytes: bytes) -> str:
    """
    Perform synchronous speech-to-text transcription using Vosk.
    
    This is a BLOCKING function executed in FastAPI's thread pool.
    It encapsulates PCM audio data, initializes a Vosk recognizer,
    and performs acoustic model inference to generate transcription.
    
    Args:
        session_id: Unique session identifier (for logging/debugging)
        pcm_bytes: Raw PCM audio data (16-bit, 16kHz, mono)
    
    Returns:
        str: Transcribed text from the audio
    
    Raises:
        RuntimeError: If Vosk model is not initialized
        Exception: If transcription fails
    """
    global VOSK_MODEL
    
    if VOSK_MODEL is None:
        raise RuntimeError(
            "Vosk model not initialized. Call initialize_vosk_model() first."
        )
    
    try:
        # Step 1: Convert raw PCM to WAV format with proper headers
        wav_bytes = create_wav_header(pcm_bytes, sample_rate=16000, channels=1, sample_width=2)
        
        # Step 2: Create in-memory WAV stream for Vosk processing
        wav_stream = io.BytesIO(wav_bytes)
        
        # Step 3: Open WAV stream and extract audio parameters
        with wave.open(wav_stream, 'rb') as wf:
            # Verify audio format matches expected parameters
            if wf.getnchannels() != 1:
                raise ValueError(f"Audio must be mono (1 channel), got {wf.getnchannels()}")
            if wf.getsampwidth() != 2:
                raise ValueError(f"Audio must be 16-bit (2 bytes), got {wf.getsampwidth()}")
            if wf.getframerate() != 16000:
                raise ValueError(f"Audio must be 16kHz, got {wf.getframerate()}")
            
            # Step 4: Initialize Vosk KaldiRecognizer for this audio stream
            # Sample rate must match the audio file
            recognizer = KaldiRecognizer(VOSK_MODEL, wf.getframerate())
            
            # Step 5: Process audio data in chunks (blocking operation)
            while True:
                # Read 4000 bytes at a time (0.125 seconds at 16kHz)
                data = wf.readframes(4000)
                if len(data) == 0:
                    break
                
                # Feed audio data to recognizer
                recognizer.AcceptWaveform(data)
            
            # Step 6: Get final transcription result
            final_result = json.loads(recognizer.FinalResult())
            transcript = final_result.get("text", "")
            
            if transcript:
                print(f"Transcription [{session_id}]: \"{transcript}\"")
            else:
                print(f"Empty transcription for session: {session_id}")
            
            return transcript
    
    except Exception as e:
        print(f"Transcription error [{session_id}]: {e}")
        # Return empty string on error rather than raising
        # This allows the conversation flow to continue
        return ""


def get_model_info() -> dict:
    """
    Get information about the loaded Vosk model.
    
    Returns:
        dict: Model metadata including path and status
    """
    return {
        "model_path": VOSK_MODEL_PATH,
        "model_loaded": VOSK_MODEL is not None,
        "sample_rate": 16000,
        "channels": 1,
        "sample_width": 2
    }

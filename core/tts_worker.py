"""
TTS Worker Module - Synchronous pyttsx3 Speech Synthesis
Handles blocking text-to-speech generation in thread pool isolation while
normalizing output to the ESP32's 16 kHz mono PCM requirement.
"""

import os
import io
import wave
import tempfile
import pyttsx3
from typing import Optional

try:
    import audioop  # type: ignore[import]
except ImportError as exc:
    raise RuntimeError("Python standard library module 'audioop' is required for TTS resampling") from exc

# TTS engine configuration
DEFAULT_RATE = 175  # Words per minute (moderate speed for clarity)

TARGET_SAMPLE_RATE = 16000  # Fixed 16 kHz to match ESP32 voice pipeline
TARGET_SAMPLE_WIDTH = 2     # 16-bit PCM
TARGET_CHANNELS = 1         # Mono playback


def _ensure_pcm_format(wav_bytes: bytes) -> bytes:
    """Normalize synthesized audio to 16 kHz, mono, 16-bit PCM."""

    try:
        with wave.open(io.BytesIO(wav_bytes), "rb") as wav_in:
            params = wav_in.getparams()
            frames = wav_in.readframes(params.nframes)
    except wave.Error as exc:
        raise ValueError(f"Invalid WAV produced by TTS engine: {exc}") from exc

    sample_width = params.sampwidth
    channels = params.nchannels
    sample_rate = params.framerate

    # Convert channels to mono if needed
    if channels > TARGET_CHANNELS:
        if channels != 2:
            raise ValueError(f"Unsupported channel count: {channels}")
        frames = audioop.tomono(frames, sample_width, 0.5, 0.5)
        channels = 1

    # Ensure 16-bit samples
    if sample_width != TARGET_SAMPLE_WIDTH:
        frames = audioop.lin2lin(frames, sample_width, TARGET_SAMPLE_WIDTH)
        sample_width = TARGET_SAMPLE_WIDTH

    # Resample to target rate if needed
    if sample_rate != TARGET_SAMPLE_RATE:
        frames, _ = audioop.ratecv(frames, sample_width, channels,
                                   sample_rate, TARGET_SAMPLE_RATE, None)
        sample_rate = TARGET_SAMPLE_RATE

    if channels != TARGET_CHANNELS or sample_width != TARGET_SAMPLE_WIDTH or sample_rate != TARGET_SAMPLE_RATE:
        raise ValueError("Failed to normalize WAV format")

    buffer = io.BytesIO()
    with wave.open(buffer, "wb") as wav_out:
        wav_out.setnchannels(TARGET_CHANNELS)
        wav_out.setsampwidth(TARGET_SAMPLE_WIDTH)
        wav_out.setframerate(TARGET_SAMPLE_RATE)
        wav_out.writeframes(frames)

    return buffer.getvalue()


def synthesize_response_audio(text: str, rate: int = DEFAULT_RATE) -> bytes:
    """
    Generate speech audio from text using pyttsx3.
    
    This is a BLOCKING function executed in FastAPI's thread pool.
    pyttsx3 requires exclusive thread control during synthesis via runAndWait().
    Uses temporary file I/O as pyttsx3 doesn't reliably support in-memory
    byte output across all platform backends (SAPI5, nsss, espeak).
    
    Args:
        text: Text content to synthesize into speech
        rate: Speech rate in words per minute (default: 175)
    
    Returns:
        bytes: Complete WAV audio file data normalized to 16 kHz mono PCM
    
    Raises:
        Exception: If synthesis fails or engine initialization fails
    
    Process:
    1. Initialize pyttsx3 engine (platform-specific backend)
    2. Configure speech properties (rate, voice)
    3. Generate unique temporary WAV file path
    4. Save synthesized speech to temp file
    5. Execute runAndWait() to block until synthesis completes
    6. Read WAV bytes from file
    7. Clean up temporary file
    8. Return audio bytes
    """
    engine = None
    temp_fd = None
    temp_path = None
    
    try:
        # Step 1: Initialize pyttsx3 engine
        # This creates a platform-specific TTS engine instance
        # Windows: SAPI5, macOS: NSSpeechSynthesizer, Linux: espeak
        engine = pyttsx3.init()
        
        # Step 2: Configure speech properties
        engine.setProperty('rate', rate)  # Speech speed
        
        # Optional: Set voice (can be customized for different languages/accents)
        # voices = engine.getProperty('voices')
        # engine.setProperty('voice', voices[0].id)  # Use first available voice
        
        # Step 3: Create temporary file for WAV output
        # mkstemp returns (file_descriptor, file_path)
        # We need the path for pyttsx3 and will manage cleanup manually
        temp_fd, temp_path = tempfile.mkstemp(suffix=".wav", prefix="hotpin_tts_")
        
        # Close the file descriptor as pyttsx3 will handle file writing
        os.close(temp_fd)
        temp_fd = None  # Mark as closed
        
        # Step 4: Queue text for synthesis and specify output file
        engine.save_to_file(text, temp_path)
        
        # Step 5: Execute synthesis (BLOCKING OPERATION)
        # This call blocks the thread until audio generation is complete
        # and the WAV file has been written to disk
        engine.runAndWait()
        
        # Step 6: Read synthesized audio bytes from temporary file
        with open(temp_path, 'rb') as audio_file:
            wav_bytes = audio_file.read()
        
        # Verify we got valid audio data
        if len(wav_bytes) == 0:
            raise ValueError("TTS synthesis produced empty audio file")

        # Normalize to device-friendly PCM format
        wav_bytes = _ensure_pcm_format(wav_bytes)
        
        print(f"✓ TTS synthesis completed: {len(wav_bytes)} bytes generated")
        
        return wav_bytes
    
    except Exception as e:
        print(f"✗ TTS synthesis error: {e}")
        raise
    
    finally:
        # Step 7: Cleanup - ensure temporary file is deleted
        if temp_path and os.path.exists(temp_path):
            try:
                os.remove(temp_path)
                print(f"✓ Cleaned up temp file: {temp_path}")
            except Exception as cleanup_error:
                print(f"⚠ Failed to cleanup temp file {temp_path}: {cleanup_error}")
        
        # Close file descriptor if still open (edge case)
        if temp_fd is not None:
            try:
                os.close(temp_fd)
            except:
                pass
        
        # Stop the TTS engine
        if engine:
            try:
                engine.stop()
            except:
                pass


def get_available_voices() -> list:
    """
    Get list of available TTS voices on the system.
    Useful for debugging and voice selection.
    
    Returns:
        list: List of voice objects with id, name, and language info
    """
    try:
        engine = pyttsx3.init()
        voices = engine.getProperty('voices')
        engine.stop()
        
        voice_info = []
        for voice in voices:
            voice_info.append({
                'id': voice.id,
                'name': voice.name,
                'languages': voice.languages if hasattr(voice, 'languages') else []
            })
        
        return voice_info
    
    except Exception as e:
        print(f"✗ Error getting voices: {e}")
        return []


def test_tts_engine() -> bool:
    """
    Test if pyttsx3 engine can be initialized successfully.
    Useful for startup validation.
    
    Returns:
        bool: True if engine initializes successfully, False otherwise
    """
    try:
        engine = pyttsx3.init()
        engine.stop()
        print("✓ pyttsx3 TTS engine test successful")
        return True
    except Exception as e:
        print(f"✗ pyttsx3 TTS engine test failed: {e}")
        return False

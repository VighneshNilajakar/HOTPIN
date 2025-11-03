# WARP.md

This file provides guidance to WARP (warp.dev) when working with code in this repository.

## Project Overview

Hotpin is a real-time voice-based conversational AI server with vision capabilities, designed for wearable hardware (ESP32-CAM). It provides a WebSocket-based audio streaming pipeline: Speech-to-Text (Vosk) → LLM (Groq with Llama 4 Scout multimodal) → Text-to-Speech (pyttsx3).

## Common Development Commands

### Setup and Installation
```powershell
# Install Python dependencies
pip install -r requirements.txt

# Download NLTK data (automatic on first run)
python -c "import nltk; nltk.download('punkt'); nltk.download('punkt_tab')"
```

### Running the Server
```powershell
# Development mode (single worker)
python main.py

# Production mode (multiple workers for high concurrency)
uvicorn main:app --host 0.0.0.0 --port 8000 --workers 4
```

### Testing
```powershell
# Test WebSocket connection health
curl http://localhost:8000/health

# Test image upload endpoint
curl -X POST http://localhost:8000/image -F "session=test-001" -F "file=@test_image.jpg"

# List available TTS voices
curl http://localhost:8000/voices
```

### ESP32 Firmware (in hotpin_esp32_firmware/)
```powershell
# Build firmware
idf.py build

# Flash to device
idf.py -p COM3 flash

# Monitor serial output
idf.py monitor
```

## Architecture Overview

### Concurrency Model (Critical)
The server uses a **hybrid async/sync concurrency model** to prevent blocking:

- **Async Operations** (event loop): WebSocket I/O, Groq API calls, session management
- **Sync Operations** (thread pool via `asyncio.to_thread()`): Vosk STT (CPU-intensive), pyttsx3 TTS (blocking file generation)

**Key Pattern**: All CPU-bound or I/O-blocking operations are offloaded to thread pools to keep the event loop responsive. Never call sync operations directly in async handlers.

### Core Pipeline Flow
```
ESP32-CAM → WebSocket → main.py (orchestrator)
                          ↓
                    ┌─────┴─────┬─────────┬────────┐
                    ↓           ↓         ↓        ↓
                STT Worker   LLM Client  TTS Worker  Session Storage
                (Vosk sync)  (Groq async)(pyttsx3 sync)(in-memory)
```

### Module Responsibilities

**`main.py`** (FastAPI orchestrator)
- WebSocket endpoint handler (`/ws`)
- Session management via in-memory dicts: `SESSION_AUDIO_BUFFERS`, `SESSION_CONTEXTS`, `SESSION_IMAGES`
- HTTP endpoints: `/image` (multimodal context), `/health`, `/voices`
- **Important**: Session state is NOT shared across multiple Uvicorn workers (use Redis for production)

**`core/llm_client.py`** (Async LLM integration)
- Groq API client using `httpx.AsyncClient` with connection pooling
- Conversation context management with sliding window (10 turns max)
- Multimodal support: `get_llm_response(session_id, transcript, image_base64=None)`
- Model: `meta-llama/llama-4-scout-17b-16e-instruct` (vision-enabled, 128K context)

**`core/stt_worker.py`** (Sync Vosk STT)
- Global Vosk model loaded once at startup (`initialize_vosk_model()`)
- Converts raw PCM → WAV with headers → Vosk KaldiRecognizer
- **Must** run in thread pool (`asyncio.to_thread()`) - blocking operation

**`core/tts_worker.py`** (Sync pyttsx3 TTS)
- Generates speech from text using platform TTS engines (SAPI5/espeak/NSSpeechSynthesizer)
- Normalizes output to 16kHz mono PCM for ESP32 compatibility using `audioop`
- Requires temporary file I/O (pyttsx3 limitation)
- **Must** run in thread pool - `engine.runAndWait()` blocks thread

### Audio Format Requirements

**Client Input (ESP32 → Server)**
- Format: Raw PCM (no WAV headers)
- Sample rate: 16000 Hz (fixed, Vosk requirement)
- Bit depth: 16-bit signed integer, little-endian
- Channels: 1 (mono)
- Chunk size: Flexible (typically 512-4096 bytes)

**Server Output (Server → ESP32)**
- Format: WAV with RIFF headers
- Sample rate: 16000 Hz (normalized by TTS worker)
- Bit depth: 16-bit
- Channels: 1 (mono, normalized)
- Chunk size: 4096 bytes (streamed)

### Session Lifecycle

1. **Connection**: Client sends handshake JSON `{"session_id": "esp32-xxx"}`
2. **Audio streaming**: Client sends binary PCM chunks
3. **End-of-speech**: Client sends `{"signal": "EOS"}`
4. **Processing**: Server runs STT → LLM (with optional image context) → TTS pipeline
5. **Response**: Server streams WAV audio chunks + completion signal
6. **Image context**: Automatically cleared after LLM response to prevent stale context
7. **Cleanup**: RESET signal (`{"signal": "RESET"}`) or disconnect clears session data

### Multimodal Vision Integration

**Image Upload Flow**:
1. ESP32-CAM posts JPEG to `/image` endpoint with session ID
2. Server converts to base64 and stores in `SESSION_IMAGES[session_id]`
3. Next voice query includes image in LLM context
4. After LLM response, image is automatically cleared

**LLM Message Format** (with vision):
```python
messages = [
    {"role": "system", "content": SYSTEM_PROMPT},
    {"role": "user", "content": [
        {"type": "text", "text": "What do you see?"},
        {"type": "image_url", "image_url": {"url": "data:image/jpeg;base64,..."}}
    ]}
]
```

## Configuration

### Environment Variables (`.env` file)
```env
GROQ_API_KEY="gsk_..."           # Required: Groq Cloud API key
VOSK_MODEL_PATH="./model"        # Path to Vosk speech model directory
SERVER_HOST="0.0.0.0"            # Server bind address
SERVER_PORT=8000                 # Server port
```

### Key Configuration Constants

**LLM Settings** (`core/llm_client.py`)
- Model: Line 27 (`GROQ_MODEL`)
- System prompt: Lines 30-36 (optimized for TTS, wearable, vision)
- Temperature: 0.2 (deterministic)
- Max tokens: 100 (enforces brevity)

**TTS Settings** (`core/tts_worker.py`)
- Speech rate: 175 WPM (Line 20)
- Target format: 16kHz mono PCM (Lines 22-24)

**STT Settings** (`core/stt_worker.py`)
- Model path: `VOSK_MODEL_PATH` env var
- Recommended models: `vosk-model-small-en-in-0.4` (40MB, fast) or `vosk-model-en-in-0.5` (1GB, accurate)

## Common Development Patterns

### Adding a New LLM Model
1. Update `GROQ_MODEL` in `core/llm_client.py` line 27
2. Adjust `max_tokens` and `temperature` if needed (lines 173-174)
3. Verify Groq API key has access to the model

### Modifying System Prompt
Edit `SYSTEM_PROMPT` in `core/llm_client.py` lines 30-36. Keep rules concise for TTS-friendly output.

### Adding Sync Operations
Always wrap blocking operations:
```python
# In main.py WebSocket handler
result = await asyncio.to_thread(blocking_function, args)
```

### Debugging WebSocket Issues
1. Check server logs for session initialization and cleanup messages
2. Use `test_client.html` for browser-based testing
3. Monitor ESP32 serial output with `idf.py monitor`
4. Capture traffic with Wireshark filter: `tcp.port == 8000 && websocket`

## Known Limitations

1. **Session storage is in-memory**: Not shared across multiple workers. Use Redis for production multi-worker deployments.
2. **TTS requires temporary files**: pyttsx3 doesn't support reliable in-memory synthesis. Cloud TTS recommended for production.
3. **No WebSocket authentication**: Add JWT or API key validation for production.
4. **Fixed 16kHz audio format**: Client must provide exactly 16kHz PCM (Vosk model constraint).
5. **Single image per session**: Multimodal context only supports one image at a time.
6. **ESP32 firmware requires flow control updates**: See `ESP32_WEBSOCKET_FIXES_REQUIRED.md` for critical client-side fixes.

## Recent Stability Improvements (Nov 2025)

### WebSocket Audio Streaming Fixes
Resolved critical buffer overflow issues causing voice mode failures:

**Server-Side (Implemented)**:
- Added 30-second inactivity timeout for audio streaming
- Improved flow control: ACK every 2 chunks (was 5) for aggressive backpressure
- Timeout on websocket.receive() to detect stale connections
- Auto-process buffered audio if EOS signal not received

**ESP32 Firmware (Required - See ESP32_WEBSOCKET_FIXES_REQUIRED.md)**:
- Increase connection stabilization: 500ms → 2000ms
- Implement backpressure: Wait for server ACK every 2 chunks
- Add 20ms delay between chunk sends to prevent burst flooding
- Fix TTS watchdog registration race condition

**Root Cause**: ESP32 was flooding WebSocket send buffer immediately after connection, causing `transport_poll_write` errors and abnormal closures (code 1006) after only 2-3 chunks.

**Testing**: After ESP32 firmware updates, expect 30+ second sustained streaming with proper EOS signal transmission.

## Testing Resources

- `TESTING_GUIDE.md`: Empty LLM response fix validation
- `VISION_TESTING_GUIDE.md`: Multimodal vision feature test scenarios
- `HOTPIN_WEBSOCKET_SPECIFICATION.md`: Complete WebSocket protocol documentation
- `test_client.html`: Browser-based WebSocket test client
- `voice_test_client.html`: Voice recording test interface

## Related Documentation

- `README.md`: Architecture overview, quick start, troubleshooting
- `VISION_IMPLEMENTATION_SUMMARY.md`: Llama 4 Scout vision integration details
- ESP32 firmware docs in `hotpin_esp32_firmware/` directory

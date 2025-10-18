# Hotpin Conversation Prototype

Real-time voice-based conversational AI server built with FastAPI, WebSockets, Vosk STT, Groq LLM, and pyttsx3 TTS.

## 🏗️ Architecture Overview

```
Client (WebSocket)
    ↓
FastAPI Server (Async Orchestrator)
    ↓
┌─────────────┬───────────────┬────────────────┐
│  STT        │   LLM         │    TTS         │
│  (Vosk)     │   (Groq)      │  (pyttsx3)     │
│  Sync/CPU   │   Async/I/O   │   Sync/CPU     │
│  Thread Pool│   Event Loop  │   Thread Pool  │
└─────────────┴───────────────┴────────────────┘
```

### Key Features

- ✅ **Real-time WebSocket audio streaming** (16-bit, 16kHz, mono PCM)
- ✅ **Offline Speech-to-Text** using Vosk (no external API required)
- ✅ **Fast LLM inference** via Groq Cloud API (llama3-8b-8192)
- ✅ **Text-to-Speech synthesis** using pyttsx3 (platform-native engines)
- ✅ **Concurrency isolation** (async I/O + sync CPU-bound thread pools)
- ✅ **Session-based conversation context** (in-memory for prototype)
- ✅ **Multi-worker deployment ready** for high concurrency

## 📋 Prerequisites

- Python 3.8+
- Vosk model (downloaded and placed in `./model` directory)
- Groq API key ([Get one here](https://console.groq.com))
- Windows: SAPI5 TTS engine (built-in)
- macOS: NSSpeechSynthesizer (built-in)
- Linux: espeak or espeak-ng (`sudo apt install espeak`)

## 🚀 Quick Start

### 1. Install Dependencies

```bash
pip install -r requirements.txt
```

### 2. Configure Environment

Edit `.env` file and add your Groq API key:

```env
GROQ_API_KEY="your_groq_api_key_here"
VOSK_MODEL_PATH="./model"
SERVER_HOST="0.0.0.0"
SERVER_PORT=8000
```

### 3. Verify Vosk Model

Ensure your Vosk model is located at:
```
./model/
```

The model directory should contain files like:
- `am/final.mdl`
- `conf/model.conf`
- `graph/HCLG.fst`
- etc.

Download models from: https://alphacephei.com/vosk/models

Recommended for English: `vosk-model-small-en-us-0.15` (40MB) or `vosk-model-en-us-0.22` (1.8GB)

### 4. Run the Server

#### Development (Single Worker):
```bash
python main.py
```

#### Production (Multiple Workers for High Concurrency):
```bash
uvicorn main:app --host 0.0.0.0 --port 8000 --workers 4
```

**Note:** With multiple workers, the in-memory session context will not be shared across workers. For production, implement Redis or PostgreSQL for shared state.

### 5. Test the Server

Open browser and navigate to:
- **API Info**: http://localhost:8000/
- **Health Check**: http://localhost:8000/health
- **Available Voices**: http://localhost:8000/voices

## 🔌 WebSocket Protocol

### Connection Flow

1. **Connect**: `ws://localhost:8000/ws`

2. **Handshake** (Client → Server):
```json
{
  "session_id": "unique-session-id"
}
```

3. **Server Acknowledgment**:
```json
{
  "status": "connected",
  "session_id": "unique-session-id"
}
```

4. **Stream Audio** (Client → Server):
   - Send raw PCM audio as binary messages
   - Format: 16-bit signed integer, 16kHz, mono
   - Chunk size: Flexible (e.g., 1024-4096 bytes)

5. **End-of-Speech Signal** (Client → Server):
```json
{
  "signal": "EOS"
}
```

6. **Processing Status Updates** (Server → Client):
```json
{
  "status": "processing",
  "stage": "transcription"
}
```

```json
{
  "status": "processing",
  "stage": "llm",
  "transcript": "user's spoken text"
}
```

```json
{
  "status": "processing",
  "stage": "tts",
  "response": "AI assistant response"
}
```

7. **Audio Response** (Server → Client):
   - Binary WAV audio chunks (4096 bytes each)
   - Continue receiving until completion signal

8. **Completion Signal**:
```json
{
  "status": "complete"
}
```

### Additional Commands

**Reset Conversation Context**:
```json
{
  "signal": "RESET"
}
```

## 📁 Project Structure

```
ESP_Warp/
├── .env                    # Environment configuration
├── requirements.txt        # Python dependencies
├── main.py                 # FastAPI application entry point
├── model/                  # Vosk speech recognition model
│   ├── am/
│   ├── conf/
│   └── graph/
└── core/
    ├── __init__.py
    ├── llm_client.py       # Groq async client + context management
    ├── stt_worker.py       # Vosk STT synchronous worker
    └── tts_worker.py       # pyttsx3 TTS synchronous worker
```

## 🔧 Configuration

### Environment Variables

| Variable | Description | Default |
|----------|-------------|---------|
| `GROQ_API_KEY` | Groq Cloud API key | (required) |
| `VOSK_MODEL_PATH` | Path to Vosk model directory | `./model` |
| `SERVER_HOST` | Server bind address | `0.0.0.0` |
| `SERVER_PORT` | Server port | `8000` |

### System Prompt

The AI assistant ("Hotpin") uses a specialized system prompt optimized for:
- **TTS-friendly output** (short sentences, plain text)
- **Indian English** dialect
- **Wearable device constraints** (15-60 words per response)
- **Action confirmations** with optional `[BEEP]` token

Edit the prompt in `core/llm_client.py` (line 30-63).

### LLM Configuration

- **Model**: `llama3-8b-8192` (Groq)
- **Temperature**: `0.2` (deterministic responses)
- **Max Tokens**: `200` (enforces brevity)

Modify in `core/llm_client.py` (line 27, 168-170).

### TTS Configuration

- **Speech Rate**: `175` words per minute
- **Voice**: System default (can be customized)

Modify in `core/tts_worker.py` (line 12).

## 🧪 Testing

### Simple Python WebSocket Client

```python
import asyncio
import json
import websockets

async def test_hotpin():
    uri = "ws://localhost:8000/ws"
    
    async with websockets.connect(uri) as websocket:
        # Handshake
        await websocket.send(json.dumps({"session_id": "test-123"}))
        response = await websocket.recv()
        print("Connected:", response)
        
        # TODO: Send PCM audio chunks here
        # with open("audio.pcm", "rb") as f:
        #     while chunk := f.read(4096):
        #         await websocket.send(chunk)
        
        # End-of-speech
        await websocket.send(json.dumps({"signal": "EOS"}))
        
        # Receive responses
        while True:
            msg = await websocket.recv()
            if isinstance(msg, str):
                data = json.loads(msg)
                print("Status:", data)
                if data.get("status") == "complete":
                    break
            else:
                print(f"Received audio chunk: {len(msg)} bytes")

asyncio.run(test_hotpin())
```

## 🎯 Performance Considerations

### Latency Optimization

The target is **<500ms** for the full pipeline (STT → LLM → TTS):

- **STT (Vosk)**: ~100-200ms (depends on model size and audio length)
- **LLM (Groq)**: ~50-150ms (Groq's specialty is low latency)
- **TTS (pyttsx3)**: ~100-300ms (depends on text length)

### Concurrency

- **Single Worker**: Suitable for 10-20 concurrent sessions
- **Multiple Workers**: Scale horizontally (4+ workers for 100+ sessions)
- **Blocking Operations**: Isolated in thread pool to prevent event loop freezing

### Memory Usage

- **Vosk Model**: 40MB - 2GB (depending on model variant)
- **Per Session**: ~1-5MB (audio buffers + conversation context)
- **Groq Client**: Connection pooled (minimal overhead)

## ⚠️ Known Limitations

### Prototype Constraints

1. **In-Memory Context Store**: 
   - Not shared across multiple Uvicorn workers
   - Lost on server restart
   - **Solution**: Use Redis or PostgreSQL for production

2. **TTS File I/O**:
   - pyttsx3 requires temporary file generation
   - Adds ~10-50ms latency
   - **Solution**: Replace with cloud TTS (Google, Azure, ElevenLabs)

3. **No Voice Activity Detection (VAD)**:
   - Client must manually signal end-of-speech
   - **Solution**: Implement client-side VAD or use streaming STT

4. **Fixed Audio Format**:
   - Requires 16-bit, 16kHz, mono PCM
   - **Solution**: Add server-side audio format conversion

## 🔐 Security Notes

- **API Keys**: Never commit `.env` file to version control
- **WebSocket Authentication**: Not implemented (add JWT or session tokens for production)
- **Input Validation**: Add rate limiting and audio size limits
- **HTTPS/WSS**: Use reverse proxy (nginx) with TLS certificates

## 📚 References

This implementation follows the architectural blueprint documented in:
`Prompt Generation for Hotpin Prototype.txt`

Key design patterns:
- FastAPI async/await concurrency model
- Thread pool offloading for CPU-bound tasks
- Connection pooling for HTTP clients
- In-memory PCM-to-WAV conversion

## 🐛 Troubleshooting

### Error: "GROQ_API_KEY not set"
- Ensure `.env` file exists and contains valid API key
- Restart the server after updating `.env`

### Error: "Vosk model not found"
- Verify model path in `.env` matches actual directory
- Check model files are complete (re-download if corrupted)

### Error: "pyttsx3 TTS engine test failed"
- **Windows**: Install SAPI5 voices (usually pre-installed)
- **Linux**: `sudo apt install espeak espeak-ng`
- **macOS**: Should work out-of-the-box

### Empty transcriptions
- Check audio format: Must be 16-bit, 16kHz, mono PCM
- Verify audio contains speech (not silence)
- Try a larger/better Vosk model

### High latency
- Use smaller Vosk model (trade accuracy for speed)
- Deploy with multiple workers: `--workers 4`
- Consider cloud STT/TTS for production

## 📝 License

This is a prototype implementation for educational/research purposes.

## 🤝 Contributing

This is a college project prototype. For production deployment, consider:
- Implementing distributed session storage (Redis)
- Adding authentication and rate limiting
- Replacing pyttsx3 with cloud TTS services
- Adding streaming STT for lower latency
- Implementing proper logging and monitoring

---

Built following the Hotpin Prototype architectural blueprint with proper concurrency isolation and real-time audio streaming.

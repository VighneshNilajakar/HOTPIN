# Hotpin WebSocket Server - Complete Technical Specification

## Document Purpose

This document provides a complete technical specification for implementing a hardware client (ESP32/HotPin device) that communicates with the Hotpin conversational AI server via WebSockets. It details the server architecture, communication protocol, data formats, and complete message flow.

---

## Table of Contents

1. [System Architecture Overview](#1-system-architecture-overview)
2. [Server Components](#2-server-components)
3. [WebSocket Communication Protocol](#3-websocket-communication-protocol)
4. [Audio Format Specifications](#4-audio-format-specifications)
5. [Complete Message Flow](#5-complete-message-flow)
6. [Client Implementation Requirements](#6-client-implementation-requirements)
7. [Error Handling](#7-error-handling)
8. [Performance Considerations](#8-performance-considerations)

---

## 1. System Architecture Overview

### 1.1 High-Level Architecture

```
┌──────────────────────────────────────────────────────────────┐
│                    Hotpin Hardware Client                     │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐       │
│  │  Microphone  │→ │  ESP32 MCU   │→ │  WiFi Module │       │
│  └──────────────┘  └──────────────┘  └──────────────┘       │
└────────────────────────────────┬─────────────────────────────┘
                                 │ WebSocket (TCP/IP)
                                 │ ws://SERVER_IP:8000/ws
                                 ↓
┌──────────────────────────────────────────────────────────────┐
│              Hotpin FastAPI WebSocket Server                  │
│  ┌──────────────────────────────────────────────────────┐   │
│  │  WebSocket Handler (main.py)                         │   │
│  │  - Session Management                                 │   │
│  │  - Audio Buffer Management                            │   │
│  │  - Pipeline Orchestration                             │   │
│  └────┬──────────────────────┬──────────────────────┬───┘   │
│       │                      │                      │        │
│       ↓                      ↓                      ↓        │
│  ┌─────────┐          ┌──────────┐          ┌──────────┐   │
│  │  Vosk   │          │  Groq    │          │ pyttsx3  │   │
│  │  STT    │          │  LLM     │          │  TTS     │   │
│  │ (Sync)  │          │ (Async)  │          │ (Sync)   │   │
│  └─────────┘          └──────────┘          └──────────┘   │
└──────────────────────────────────────────────────────────────┘
                                 │
                                 ↓
                    ┌────────────────────┐
                    │   Speaker Output   │
                    │ (WAV Audio Stream) │
                    └────────────────────┘
```

### 1.2 Server Technology Stack

| Component | Technology | Purpose | Execution Model |
|-----------|-----------|---------|-----------------|
| **Web Framework** | FastAPI 0.104+ | ASGI web server | Async event loop |
| **WebSocket** | FastAPI WebSocket | Bidirectional communication | Async I/O |
| **STT Engine** | Vosk (offline) | Speech-to-Text | Sync (thread pool) |
| **LLM Provider** | Groq Cloud API | Conversational AI | Async HTTP |
| **TTS Engine** | pyttsx3 | Text-to-Speech | Sync (thread pool) |
| **HTTP Client** | httpx.AsyncClient | LLM API calls | Connection pooled |

### 1.3 Concurrency Model

The server uses a **hybrid concurrency model** to prevent blocking:

- **Async Operations** (Event Loop):
  - WebSocket I/O (receive/send messages)
  - Groq API HTTP requests
  - Session state management

- **Sync Operations** (Thread Pool):
  - Vosk speech recognition (CPU-intensive)
  - pyttsx3 speech synthesis (blocks during generation)

**Critical**: The server uses `asyncio.to_thread()` to offload blocking operations, ensuring the event loop remains responsive.

---

## 2. Server Components

### 2.1 Main Application (`main.py`)

#### 2.1.1 Application Lifecycle

**Startup Sequence:**
```python
1. Load environment variables (.env file)
2. Initialize FastAPI app with lifespan handler
3. On startup event:
   a. Initialize Groq AsyncClient (connection pooled)
   b. Load Vosk speech recognition model (global)
   c. Test pyttsx3 TTS engine
4. WebSocket endpoint ready at ws://HOST:PORT/ws
```

**Shutdown Sequence:**
```python
1. Receive shutdown signal (CTRL+C or SIGTERM)
2. Close Groq AsyncClient (gracefully)
3. Clear all session buffers and contexts
4. Cleanup complete
```

#### 2.1.2 Session Management

**In-Memory Storage:**
```python
# Audio buffers (main.py)
SESSION_AUDIO_BUFFERS: Dict[str, io.BytesIO] = {}
# session_id → BytesIO buffer for PCM audio

# Conversation contexts (core/llm_client.py)
SESSION_CONTEXTS: Dict[str, dict] = {
    "session_id": {
        "history": [
            {"role": "user", "content": "..."},
            {"role": "assistant", "content": "..."}
        ],
        "last_activity_ts": 1234567890.0
    }
}
```

**Important**: With multiple Uvicorn workers, session state is **NOT shared**. For production, use Redis or PostgreSQL.

### 2.2 STT Module (`core/stt_worker.py`)

#### 2.2.1 Vosk Model Loading

```python
# Global model (loaded once at startup)
VOSK_MODEL: Optional[Model] = None
VOSK_MODEL_PATH: str = os.getenv("VOSK_MODEL_PATH", "./model")

def initialize_vosk_model() -> None:
    """Loads Vosk model into memory (14MB - 2GB depending on variant)"""
    global VOSK_MODEL
    VOSK_MODEL = Model(VOSK_MODEL_PATH)
```

**Model Requirements:**
- Must be a valid Vosk model directory
- Contains: `am/`, `conf/`, `graph/`, `ivector/` subdirectories
- Sample rate: 16kHz (matches client audio format)

#### 2.2.2 Audio Processing Pipeline

```python
def process_audio_for_transcription(session_id: str, pcm_bytes: bytes) -> str:
    """
    BLOCKING function - runs in thread pool
    
    Steps:
    1. Convert raw PCM → WAV format (in-memory)
    2. Create KaldiRecognizer with global model
    3. Process audio in 4000-byte chunks
    4. Return final transcript text
    """
```

**Audio Conversion:**
```python
def create_wav_header(pcm_data: bytes, 
                     sample_rate: int = 16000,
                     channels: int = 1, 
                     sample_width: int = 2) -> bytes:
    """
    Wraps raw PCM with WAV headers using Python's wave module.
    
    WAV Format:
    - RIFF header + fmt chunk + data chunk
    - PCM signed 16-bit little-endian
    - Mono channel
    - 16kHz sample rate
    """
```

### 2.3 LLM Module (`core/llm_client.py`)

#### 2.3.1 Groq Client Management

```python
groq_client: Optional[httpx.AsyncClient] = None

def init_client() -> None:
    """Initialize connection-pooled HTTP client"""
    groq_client = httpx.AsyncClient(
        base_url="https://api.groq.com/openai/v1",
        headers={
            "Authorization": f"Bearer {GROQ_API_KEY}",
            "Content-Type": "application/json"
        },
        timeout=30.0
    )
```

**API Configuration:**
```python
GROQ_MODEL = "llama3-8b-8192"
TEMPERATURE = 0.2  # Deterministic responses
MAX_TOKENS = 200   # Enforces brevity (15-60 words)
```

#### 2.3.2 Context Management

```python
def manage_context(session_id: str, role: str, content: str, 
                   max_history_turns: int = 10) -> None:
    """
    Maintains conversation history with sliding window.
    
    Max turns: 10 (= 20 messages total: 10 user + 10 assistant)
    Older messages are automatically discarded.
    """
```

### 2.4 TTS Module (`core/tts_worker.py`)

#### 2.4.1 Speech Synthesis

```python
def synthesize_response_audio(text: str, rate: int = 175) -> bytes:
    """
    BLOCKING function - runs in thread pool
    
    Steps:
    1. Initialize pyttsx3 engine (platform-specific)
    2. Set speech rate (175 words per minute)
    3. Create temporary WAV file
    4. Call engine.runAndWait() [BLOCKS THREAD]
    5. Read WAV bytes from file
    6. Delete temporary file
    7. Return WAV bytes
    """
```

**TTS Configuration:**
- Speech rate: 175 WPM (moderate speed)
- Voice: System default (Windows: SAPI5, Linux: espeak)
- Output format: WAV (PCM, varies by engine)

---

## 3. WebSocket Communication Protocol

### 3.1 Connection Establishment

**Step 1: Client Initiates Connection**
```
Client → Server: TCP SYN (WebSocket handshake)
URL: ws://SERVER_IP:8000/ws
```

**Step 2: Server Accepts Connection**
```python
@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    await websocket.accept()  # HTTP 101 Switching Protocols
```

**Step 3: Client Sends Handshake (JSON)**
```json
{
  "session_id": "unique-device-id"
}
```
- **Data Type**: Text (JSON string)
- **Encoding**: UTF-8
- **Required**: Yes (first message)

**Step 4: Server Acknowledges**
```json
{
  "status": "connected",
  "session_id": "unique-device-id"
}
```

### 3.2 Message Types

The protocol uses **TWO message types**:

| Type | Purpose | Direction | Format |
|------|---------|-----------|--------|
| **Text** | Control signals, status updates | Bidirectional | JSON (UTF-8) |
| **Binary** | Audio data | Bidirectional | Raw bytes |

### 3.3 Client → Server Messages

#### 3.3.1 Handshake Message (Text)
```json
{
  "session_id": "esp32-hotpin-001"
}
```
- **When**: First message after connection
- **Purpose**: Initialize session and create audio buffer

#### 3.3.2 Audio Data (Binary)
```
Raw PCM audio bytes (16-bit signed integer, little-endian)
Sample rate: 16kHz
Channels: 1 (mono)
```
- **When**: During voice recording
- **Purpose**: Stream microphone data to server
- **Chunk Size**: Flexible (recommended: 512-4096 bytes)
- **Encoding**: Raw binary (no WAV headers needed)

**Example Frame Structure:**
```
[sample1_low] [sample1_high] [sample2_low] [sample2_high] ...
Each sample: 2 bytes (int16_t, little-endian, range: -32768 to 32767)
```

#### 3.3.3 End-of-Speech Signal (Text)
```json
{
  "signal": "EOS"
}
```
- **When**: User finished speaking
- **Purpose**: Trigger STT → LLM → TTS pipeline
- **Critical**: Must be sent after all audio data

#### 3.3.4 Reset Signal (Text)
```json
{
  "signal": "RESET"
}
```
- **When**: Clear conversation history (optional)
- **Purpose**: Start fresh conversation

### 3.4 Server → Client Messages

#### 3.4.1 Connection Acknowledgment (Text)
```json
{
  "status": "connected",
  "session_id": "esp32-hotpin-001"
}
```

#### 3.4.2 Processing Status Updates (Text)

**Transcription Stage:**
```json
{
  "status": "processing",
  "stage": "transcription"
}
```

**LLM Stage:**
```json
{
  "status": "processing",
  "stage": "llm",
  "transcript": "hello how are you"
}
```
- **transcript**: What Vosk heard (user's speech)

**TTS Stage:**
```json
{
  "status": "processing",
  "stage": "tts",
  "response": "Hello! I'm fine, thank you. How can I help you today?"
}
```
- **response**: AI's text response (before TTS)

#### 3.4.3 Audio Response (Binary)
```
WAV audio data (chunked)
Chunk size: 4096 bytes
Format: WAV file format (includes headers)
```
- **When**: After TTS synthesis complete
- **Purpose**: AI voice response
- **Total size**: Varies (typically 100KB - 500KB)
- **Playback**: Client must reassemble chunks and play as WAV

**WAV Format Details:**
```
RIFF Header (44 bytes) + PCM audio data
Channels: 1 or 2 (depends on TTS engine)
Sample rate: Varies (typically 22050 Hz or 16000 Hz)
Bit depth: 16-bit
```

#### 3.4.4 Completion Signal (Text)
```json
{
  "status": "complete"
}
```
- **When**: All audio chunks sent
- **Purpose**: Client can now start playing audio

#### 3.4.5 Error Messages (Text)
```json
{
  "status": "error",
  "message": "Could not understand audio. Please try again."
}
```

**Error Types:**
- Empty audio buffer
- Empty transcription
- STT failure
- LLM API failure
- TTS synthesis failure

---

## 4. Audio Format Specifications

### 4.1 Client Input Audio (Microphone → Server)

**MANDATORY Format:**

| Parameter | Value | Notes |
|-----------|-------|-------|
| **Format** | Raw PCM | No WAV headers needed |
| **Sample Rate** | 16000 Hz | Fixed (Vosk model requirement) |
| **Bit Depth** | 16-bit | Signed integer |
| **Encoding** | Little-endian | Standard for most hardware |
| **Channels** | 1 (Mono) | Single microphone |
| **Data Type** | `int16_t` | Range: -32768 to +32767 |

**Calculation Examples:**

1. **Bytes per Second:**
   ```
   16000 samples/sec × 2 bytes/sample × 1 channel = 32,000 bytes/sec
   ```

2. **1 Second of Audio:**
   ```
   32,000 bytes = 32 KB
   ```

3. **Recommended Chunk Size:**
   ```
   512 bytes = 16 ms of audio
   1024 bytes = 32 ms of audio
   4096 bytes = 128 ms of audio
   ```

**ESP32 Audio Capture Example:**
```c
// Using I2S microphone on ESP32
#define SAMPLE_RATE 16000
#define BITS_PER_SAMPLE 16
#define CHANNELS 1
#define DMA_BUFFER_SIZE 1024  // 32ms chunks

int16_t audio_buffer[DMA_BUFFER_SIZE];

// Read from I2S peripheral
size_t bytes_read;
i2s_read(I2S_NUM_0, audio_buffer, sizeof(audio_buffer), 
         &bytes_read, portMAX_DELAY);

// Send directly to WebSocket (no conversion needed)
websocket_send_binary((uint8_t*)audio_buffer, bytes_read);
```

### 4.2 Server Output Audio (Server → Speaker)

**WAV File Format:**

| Parameter | Value | Notes |
|-----------|-------|-------|
| **Container** | WAV (RIFF) | Includes 44-byte header |
| **Sample Rate** | Varies | Typically 16kHz or 22.05kHz |
| **Bit Depth** | 16-bit | Signed integer |
| **Channels** | 1 or 2 | Depends on TTS engine |
| **Chunk Size** | 4096 bytes | Streamed incrementally |

**WAV Header Structure:**
```c
struct WAVHeader {
    char riff[4];           // "RIFF"
    uint32_t file_size;     // Total file size - 8
    char wave[4];           // "WAVE"
    char fmt[4];            // "fmt "
    uint32_t fmt_size;      // 16
    uint16_t audio_format;  // 1 (PCM)
    uint16_t channels;      // 1 or 2
    uint32_t sample_rate;   // 16000 or 22050
    uint32_t byte_rate;     // sample_rate × channels × (bits/8)
    uint16_t block_align;   // channels × (bits/8)
    uint16_t bits;          // 16
    char data[4];           // "data"
    uint32_t data_size;     // PCM data size
};
```

**Client Processing:**
```c
// Receive all binary chunks
uint8_t wav_buffer[MAX_WAV_SIZE];
size_t total_bytes = 0;

while (receiving_audio) {
    size_t chunk_size;
    websocket_receive_binary(chunk_buffer, &chunk_size);
    memcpy(wav_buffer + total_bytes, chunk_buffer, chunk_size);
    total_bytes += chunk_size;
}

// Parse WAV header
WAVHeader* header = (WAVHeader*)wav_buffer;
uint8_t* pcm_data = wav_buffer + sizeof(WAVHeader);

// Send to I2S DAC/speaker
i2s_write(I2S_NUM_0, pcm_data, header->data_size, 
          &bytes_written, portMAX_DELAY);
```

---

## 5. Complete Message Flow

### 5.1 Full Conversation Cycle

```
┌─────────────┐                                    ┌─────────────┐
│   Client    │                                    │   Server    │
│  (ESP32)    │                                    │  (FastAPI)  │
└──────┬──────┘                                    └──────┬──────┘
       │                                                  │
       │  1. WebSocket Handshake (HTTP → WS)             │
       ├─────────────────────────────────────────────────>│
       │                                                  │
       │  2. Accept Connection (101 Switching Protocols) │
       │<─────────────────────────────────────────────────┤
       │                                                  │
       │  3. Send Handshake JSON                         │
       │     {"session_id": "esp32-001"}                 │
       ├─────────────────────────────────────────────────>│
       │                                                  │─┐
       │                                                  │ │ Initialize
       │                                                  │ │ session buffer
       │                                                  │<┘
       │  4. Acknowledgment                              │
       │     {"status": "connected", ...}                │
       │<─────────────────────────────────────────────────┤
       │                                                  │
       │  5. Start Recording (button press)              │
       │                                                  │
       │  6. Stream PCM Audio Chunks (binary)            │
       │     [chunk1: 1024 bytes]                        │
       ├─────────────────────────────────────────────────>│
       │     [chunk2: 1024 bytes]                        │─┐
       ├─────────────────────────────────────────────────>│ │ Buffer PCM
       │     [chunk3: 1024 bytes]                        │ │ in memory
       ├─────────────────────────────────────────────────>│<┘
       │     ... (continue streaming)                    │
       │                                                  │
       │  7. Stop Recording & Send EOS                   │
       │     {"signal": "EOS"}                           │
       ├─────────────────────────────────────────────────>│
       │                                                  │
       │  8. Status: Transcription                       │─┐
       │     {"status": "processing", "stage": "trans.."}│ │ STT
       │<─────────────────────────────────────────────────┤ │ (Vosk)
       │                                                  │<┘
       │  9. Status: LLM                                 │
       │     {"status": "processing", "stage": "llm",    │─┐
       │      "transcript": "hello hotpin"}              │ │ LLM
       │<─────────────────────────────────────────────────┤ │ (Groq)
       │                                                  │<┘
       │  10. Status: TTS                                │
       │     {"status": "processing", "stage": "tts",    │─┐
       │      "response": "Hello! How can I help?"}      │ │ TTS
       │<─────────────────────────────────────────────────┤ │ (pyttsx3)
       │                                                  │<┘
       │  11. WAV Audio Stream (binary chunks)           │
       │     [wav_chunk1: 4096 bytes]                    │
       │<─────────────────────────────────────────────────┤
       │     [wav_chunk2: 4096 bytes]                    │─┐
       │<─────────────────────────────────────────────────┤ │ Play audio
       │     ... (continue receiving)                    │ │ via I2S DAC
       │     [wav_chunkN: 1234 bytes]                    │<┘
       │<─────────────────────────────────────────────────┤
       │                                                  │
       │  12. Completion Signal                          │
       │     {"status": "complete"}                      │
       │<─────────────────────────────────────────────────┤
       │                                                  │
       │  (Ready for next conversation turn)             │
       │                                                  │
```

### 5.2 Timing Expectations

**Typical Latency Breakdown:**

| Stage | Duration | Notes |
|-------|----------|-------|
| **Network RTT** | 10-50 ms | WiFi + Internet |
| **STT (Vosk)** | 100-300 ms | Depends on audio length & model |
| **LLM (Groq)** | 50-200 ms | Groq's low-latency specialty |
| **TTS (pyttsx3)** | 100-400 ms | Depends on text length |
| **Audio Streaming** | 50-100 ms | Chunked transmission |
| **Total Pipeline** | 300-1050 ms | Target: <500ms for short phrases |

**Optimization Tips:**
- Use smaller Vosk model (40MB vs 2GB)
- Shorter audio clips (3-5 seconds optimal)
- Local network reduces RTT
- Multiple Uvicorn workers for concurrency

---

## 6. Client Implementation Requirements

### 6.1 Hardware Requirements

**Minimum ESP32 Specifications:**

| Component | Requirement | Recommendation |
|-----------|-------------|----------------|
| **MCU** | ESP32 (dual-core) | ESP32-S3 preferred |
| **RAM** | 320 KB minimum | 512 KB+ for buffering |
| **Flash** | 4 MB minimum | 8 MB+ for OTA updates |
| **WiFi** | 2.4 GHz 802.11 b/g/n | Good signal strength |
| **Microphone** | I2S MEMS (16kHz capable) | INMP441, SPH0645 |
| **Speaker/DAC** | I2S DAC or PWM | MAX98357A I2S amp |

### 6.2 Software Requirements

**Required Libraries (ESP32 Arduino/ESP-IDF):**

```cpp
// Network
#include <WiFi.h>
#include <WebSocketsClient.h>  // markusSattler library

// Audio
#include <driver/i2s.h>

// JSON parsing
#include <ArduinoJson.h>

// Optional: Audio processing
#include <ESP32-audioI2S.h>  // Schreibfaul1 library
```

### 6.3 Implementation Pseudocode

#### 6.3.1 Setup Phase

```cpp
void setup() {
    // 1. Initialize WiFi
    WiFi.begin(SSID, PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
        delay(100);
    }
    
    // 2. Initialize I2S microphone
    i2s_config_t i2s_config_mic = {
        .mode = I2S_MODE_MASTER | I2S_MODE_RX,
        .sample_rate = 16000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        // ... additional config
    };
    i2s_driver_install(I2S_NUM_0, &i2s_config_mic, 0, NULL);
    
    // 3. Initialize I2S speaker/DAC
    i2s_config_t i2s_config_spk = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX,
        .sample_rate = 16000,  // Adjustable based on received WAV
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        // ... additional config
    };
    i2s_driver_install(I2S_NUM_1, &i2s_config_spk, 0, NULL);
    
    // 4. Connect WebSocket
    webSocket.begin(SERVER_IP, 8000, "/ws");
    webSocket.onEvent(webSocketEvent);
    
    // 5. Initialize state machine
    state = STATE_DISCONNECTED;
}
```

#### 6.3.2 WebSocket Event Handler

```cpp
void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch(type) {
        case WStype_CONNECTED:
            Serial.println("WebSocket Connected");
            // Send handshake
            sendHandshake();
            state = STATE_IDLE;
            break;
            
        case WStype_TEXT:
            // Parse JSON message
            handleTextMessage((char*)payload);
            break;
            
        case WStype_BIN:
            // Receive audio chunk
            handleAudioChunk(payload, length);
            break;
            
        case WStype_DISCONNECTED:
            Serial.println("WebSocket Disconnected");
            state = STATE_DISCONNECTED;
            break;
    }
}
```

#### 6.3.3 Audio Recording & Transmission

```cpp
void recordAndSendAudio() {
    const size_t CHUNK_SIZE = 1024;  // 32ms chunks
    int16_t audio_buffer[CHUNK_SIZE/2];
    size_t bytes_read;
    
    state = STATE_RECORDING;
    Serial.println("Recording started");
    
    while (buttonPressed && state == STATE_RECORDING) {
        // Read from I2S microphone
        i2s_read(I2S_NUM_0, audio_buffer, CHUNK_SIZE, 
                 &bytes_read, portMAX_DELAY);
        
        // Send binary chunk to server
        webSocket.sendBIN((uint8_t*)audio_buffer, bytes_read);
        
        // Optional: LED indicator
        blinkLED();
    }
    
    // Send EOS signal
    webSocket.sendTXT("{\"signal\":\"EOS\"}");
    state = STATE_PROCESSING;
    Serial.println("Recording stopped, processing...");
}
```

#### 6.3.4 Audio Playback

```cpp
#define MAX_WAV_SIZE 512000  // 512KB buffer

uint8_t wav_buffer[MAX_WAV_SIZE];
size_t wav_size = 0;
bool receiving_audio = false;

void handleAudioChunk(uint8_t* data, size_t length) {
    if (!receiving_audio) {
        // First chunk
        receiving_audio = true;
        wav_size = 0;
    }
    
    // Append chunk to buffer
    if (wav_size + length < MAX_WAV_SIZE) {
        memcpy(wav_buffer + wav_size, data, length);
        wav_size += length;
    } else {
        Serial.println("WAV buffer overflow!");
    }
}

void handleTextMessage(char* json) {
    StaticJsonDocument<512> doc;
    deserializeJson(doc, json);
    
    const char* status = doc["status"];
    
    if (strcmp(status, "processing") == 0) {
        const char* stage = doc["stage"];
        
        if (strcmp(stage, "tts") == 0) {
            // Start receiving audio
            receiving_audio = true;
            wav_size = 0;
        }
        
        // Optional: Display transcript/response on OLED
        if (doc.containsKey("transcript")) {
            Serial.print("You: ");
            Serial.println(doc["transcript"].as<const char*>());
        }
        if (doc.containsKey("response")) {
            Serial.print("Hotpin: ");
            Serial.println(doc["response"].as<const char*>());
        }
    }
    else if (strcmp(status, "complete") == 0) {
        // Play received audio
        playWAVAudio(wav_buffer, wav_size);
        state = STATE_IDLE;
    }
    else if (strcmp(status, "error") == 0) {
        Serial.println(doc["message"].as<const char*>());
        state = STATE_IDLE;
    }
}

void playWAVAudio(uint8_t* wav_data, size_t size) {
    // Parse WAV header
    WAVHeader* header = (WAVHeader*)wav_data;
    uint8_t* pcm_data = wav_data + sizeof(WAVHeader);
    size_t pcm_size = size - sizeof(WAVHeader);
    
    // Adjust I2S sample rate if needed
    if (header->sample_rate != 16000) {
        i2s_set_sample_rates(I2S_NUM_1, header->sample_rate);
    }
    
    // Play audio
    size_t bytes_written;
    i2s_write(I2S_NUM_1, pcm_data, pcm_size, &bytes_written, portMAX_DELAY);
    
    Serial.println("Audio playback complete");
}
```

### 6.4 State Machine

```cpp
enum State {
    STATE_DISCONNECTED,  // No WebSocket connection
    STATE_CONNECTING,    // Establishing connection
    STATE_IDLE,         // Connected, waiting for button press
    STATE_RECORDING,    // Recording and streaming audio
    STATE_PROCESSING,   // Server processing (STT→LLM→TTS)
    STATE_PLAYING       // Playing TTS response
};

State state = STATE_DISCONNECTED;
```

**State Transitions:**
```
DISCONNECTED → CONNECTING → IDLE
     ↑                       ↓
     └───────────────────────┘ (button press)
                             ↓
                        RECORDING
                             ↓
                        PROCESSING
                             ↓
                         PLAYING
                             ↓
                          IDLE
```

---

## 7. Error Handling

### 7.1 Server-Side Errors

**Error Types & Recovery:**

| Error | Cause | Server Response | Client Action |
|-------|-------|-----------------|---------------|
| **Empty Audio** | No PCM data buffered | `{"status":"error", "message":"Empty audio buffer"}` | Record again |
| **Empty Transcript** | Vosk returned no text | `{"status":"error", "message":"Could not understand audio"}` | Retry with clearer speech |
| **STT Failure** | Vosk exception | Logged server-side, returns empty string | Retry or reset |
| **LLM Timeout** | Groq API slow/down | `"Service temporarily unavailable"` | Wait and retry |
| **TTS Failure** | pyttsx3 exception | Logged server-side | Skip audio playback |

### 7.2 Client-Side Error Handling

**Recommended Client Checks:**

```cpp
// 1. WiFi Connection
if (WiFi.status() != WL_CONNECTED) {
    reconnectWiFi();
}

// 2. WebSocket Connection
if (!webSocket.isConnected()) {
    webSocket.begin(SERVER_IP, 8000, "/ws");
}

// 3. Audio Buffer Overflow
if (wav_size + chunk_size > MAX_WAV_SIZE) {
    Serial.println("Buffer overflow - discarding audio");
    wav_size = 0;
    receiving_audio = false;
}

// 4. Timeout Handling
unsigned long timeout_start = millis();
while (state == STATE_PROCESSING) {
    if (millis() - timeout_start > 10000) {  // 10s timeout
        Serial.println("Server timeout");
        state = STATE_IDLE;
        break;
    }
    webSocket.loop();
    delay(10);
}
```

---

## 8. Performance Considerations

### 8.1 Network Optimization

**WiFi Connection:**
- Use static IP for faster connection
- Monitor RSSI (signal strength)
- Implement exponential backoff for reconnection

**WebSocket:**
- Keep connection alive (no frequent reconnects)
- Use binary frames for audio (not Base64-encoded text)
- Reuse TCP connection for multiple conversations

### 8.2 Memory Management

**ESP32 Memory Constraints:**

| Buffer | Size | Purpose |
|--------|------|---------|
| **Tx Audio** | 64-128 KB | Mic recording buffer (2-4 seconds) |
| **Rx Audio** | 256-512 KB | TTS response buffer |
| **JSON** | 512 bytes | Status messages |
| **WebSocket** | 8-16 KB | Frame buffer |

**Memory Optimization:**
```cpp
// Use PSRAM if available (ESP32-WROVER)
#ifdef BOARD_HAS_PSRAM
    uint8_t* wav_buffer = (uint8_t*)ps_malloc(MAX_WAV_SIZE);
#else
    uint8_t wav_buffer[MAX_WAV_SIZE];
#endif

// Free memory after playback
if (wav_buffer_dynamic) {
    free(wav_buffer_dynamic);
}
```

### 8.3 Audio Quality vs. Bandwidth

**Trade-offs:**

| Sample Rate | Bandwidth | Quality | Recommendation |
|-------------|-----------|---------|----------------|
| 8 kHz | 16 KB/s | Telephone | Not recommended |
| **16 kHz** | **32 KB/s** | **Good voice** | **Optimal** |
| 22.05 kHz | 44 KB/s | High quality | Unnecessary overhead |
| 44.1 kHz | 88 KB/s | Music quality | Not supported |

**Recommendation**: **Stick to 16 kHz** - perfect balance for voice recognition and bandwidth.

### 8.4 Power Consumption

**Optimization Strategies:**
1. **Sleep during idle**: Deep sleep when not recording
2. **WiFi power save**: Use `WiFi.setSleep(WIFI_PS_MIN_MODEM)`
3. **Disable Bluetooth**: If not needed
4. **Lower I2S clock**: When not recording
5. **Use button interrupt**: Wake on button press

---

## 9. Testing & Debugging

### 9.1 Server Logs

**Key Log Messages:**
```
✓ Groq AsyncClient initialized with model: llama3-8b-8192
✓ Vosk model loaded successfully
✓ Session initialized: esp32-001
🎤 End-of-speech signal received
🔄 Processing X bytes of audio...
✓ Transcription: "hello hotpin"
🤖 LLM response: "Hello! How can I help?"
✓ TTS synthesis completed: X bytes generated
🔊 Streaming X bytes of audio response...
✓ Response streaming complete
```

### 9.2 Client Debugging

**Serial Monitor Output:**
```cpp
Serial.println("=== Hotpin Client Debug ===");
Serial.printf("WiFi: %s (RSSI: %d dBm)\n", 
              WiFi.localIP().toString().c_str(), 
              WiFi.RSSI());
Serial.printf("WebSocket: %s\n", 
              webSocket.isConnected() ? "Connected" : "Disconnected");
Serial.printf("State: %d\n", state);
Serial.printf("Audio buffer: %d bytes\n", wav_size);
```

### 9.3 Wireshark Analysis

**WebSocket Packet Capture:**
```
Filter: tcp.port == 8000 && websocket

Expected packets:
1. HTTP Upgrade Request (GET /ws)
2. HTTP 101 Switching Protocols
3. WebSocket Text Frame: {"session_id":"..."}
4. WebSocket Text Frame: {"status":"connected",...}
5. WebSocket Binary Frame: [PCM audio data]
6. WebSocket Binary Frame: [PCM audio data]
   ... (multiple)
7. WebSocket Text Frame: {"signal":"EOS"}
8. WebSocket Text Frame: {"status":"processing",...}
   ... (status updates)
9. WebSocket Binary Frame: [WAV audio chunk]
   ... (multiple)
10. WebSocket Text Frame: {"status":"complete"}
```

---

## 10. Example ESP32 Client Architecture

### 10.1 File Structure

```
hotpin_esp32_client/
├── src/
│   ├── main.cpp              # Main application
│   ├── audio_handler.cpp     # I2S audio recording/playback
│   ├── websocket_handler.cpp # WebSocket communication
│   ├── state_machine.cpp     # State management
│   └── config.h              # Configuration constants
├── lib/
│   ├── WebSocketsClient/     # WebSocket library
│   └── ArduinoJson/          # JSON parsing
└── platformio.ini            # PlatformIO config
```

### 10.2 Configuration Constants

```cpp
// config.h
#define WIFI_SSID "YourWiFiSSID"
#define WIFI_PASSWORD "YourPassword"
#define SERVER_IP "192.168.1.100"  // Your PC's IP
#define SERVER_PORT 8000
#define SESSION_ID "esp32-hotpin-001"

// Audio settings
#define SAMPLE_RATE 16000
#define BITS_PER_SAMPLE 16
#define CHANNELS 1
#define DMA_BUFFER_SIZE 1024

// I2S pins (adjust for your board)
#define I2S_MIC_SCK 26   // BCLK
#define I2S_MIC_WS  25   // LRCLK
#define I2S_MIC_SD  33   // DOUT

#define I2S_SPK_SCK 14   // BCLK
#define I2S_SPK_WS  27   // LRCLK
#define I2S_SPK_SD  12   // DIN

// Button
#define BUTTON_PIN 0     // GPIO0 (BOOT button)
```

---

## 11. Security Considerations

### 11.1 Current Implementation (Development)

**Protocol**: `ws://` (WebSocket over plain TCP)
- ❌ **No encryption** - traffic visible on network
- ❌ **No authentication** - anyone can connect
- ✅ **Suitable for**: Local development, trusted networks

### 11.2 Production Recommendations

**1. Use WSS (WebSocket Secure):**
```
wss://SERVER_IP:8443/ws  # TLS-encrypted WebSocket
```

**2. Add Authentication:**
```json
// Handshake with API key
{
  "session_id": "esp32-001",
  "api_key": "your-secret-key"
}
```

**3. Server-Side Validation:**
```python
# main.py
API_KEYS = os.getenv("API_KEYS").split(",")

handshake_data = json.loads(handshake_message)
if handshake_data.get("api_key") not in API_KEYS:
    await websocket.close(code=1008, reason="Invalid API key")
    return
```

**4. Rate Limiting:**
- Limit connections per IP
- Limit audio upload size
- Timeout inactive sessions

---

## 12. Appendix

### 12.1 Server Configuration (.env)

```env
# Required
GROQ_API_KEY="gsk_xxxxxxxxxxxxxxxxxxxxxxxxxxxx"
VOSK_MODEL_PATH="./model"

# Optional
SERVER_HOST="0.0.0.0"  # Listen on all interfaces
SERVER_PORT=8000       # Default WebSocket port
```

### 12.2 Server Launch Commands

**Development (Single Worker):**
```bash
python main.py
```

**Production (Multi-Worker):**
```bash
uvicorn main:app --host 0.0.0.0 --port 8000 --workers 4
```

**With SSL (Production):**
```bash
uvicorn main:app --host 0.0.0.0 --port 8443 \
  --ssl-keyfile=key.pem --ssl-certfile=cert.pem --workers 4
```

### 12.3 Vosk Model Selection

**Recommended Models:**

| Model | Size | Language | Accuracy | Latency |
|-------|------|----------|----------|---------|
| `vosk-model-small-en-in-0.4` | 40 MB | Indian English | Good | Fast |
| `vosk-model-en-in-0.5` | 1 GB | Indian English | Excellent | Moderate |
| `vosk-model-small-en-us-0.15` | 40 MB | US English | Good | Fast |
| `vosk-model-en-us-0.22` | 1.8 GB | US English | Excellent | Slower |

**Download**: https://alphacephei.com/vosk/models

### 12.4 Groq Model Options

**Available Models:**

| Model | Context | Speed | Quality |
|-------|---------|-------|---------|
| `llama3-8b-8192` | 8K tokens | Very Fast | Good |
| `llama3-70b-8192` | 8K tokens | Fast | Excellent |
| `mixtral-8x7b-32768` | 32K tokens | Fast | Excellent |
| `gemma-7b-it` | 8K tokens | Very Fast | Good |

**Current**: `llama3-8b-8192` (optimal speed/quality balance)

---

## 13. Frequently Asked Questions

### Q1: Can I use WAV files instead of raw PCM from the client?
**A:** No. The server expects **raw PCM bytes** without WAV headers. The server adds headers internally before passing to Vosk.

### Q2: What if my ESP32 can only do 8kHz sampling?
**A:** The server requires **16kHz**. You must either:
- Use hardware that supports 16kHz
- Implement resampling on ESP32 (CPU-intensive)
- Modify server to accept multiple rates (not recommended)

### Q3: How do I handle multiple conversations without reconnecting?
**A:** Keep the WebSocket open and repeat the recording cycle:
```
IDLE → RECORDING → PROCESSING → PLAYING → IDLE → ...
```
No need to disconnect between conversations.

### Q4: Can I stream TTS audio in real-time as it generates?
**A:** No. pyttsx3 generates the entire audio file before returning. For streaming TTS, you'd need to replace pyttsx3 with a cloud service (Google TTS, Azure, ElevenLabs).

### Q5: What's the maximum audio length?
**A:** No hard limit, but optimal is **3-10 seconds**:
- <3s: May not contain complete phrases
- 3-10s: Optimal for conversational turns
- >10s: Increases latency and memory usage

### Q6: How do I add custom wake word detection?
**A:** Implement on ESP32 client:
```cpp
// Use edge_impulse or Porcupine for wake word detection
if (detectWakeWord()) {
    startRecordingAndSendToServer();
}
```
The server doesn't handle wake word detection.

### Q7: Can multiple devices connect simultaneously?
**A:** Yes, with unique `session_id` values. Each session is isolated.

**Limitation**: With multiple Uvicorn workers, sessions aren't shared. Use Redis for production.

---

## 14. Conclusion

This specification provides everything needed to build a hardware client for the Hotpin conversational AI server. Key takeaways:

✅ **Use WebSocket** with text (JSON) and binary (audio) frames  
✅ **Send raw 16kHz PCM audio** (no WAV headers needed from client)  
✅ **Handle three message types**: handshake, audio data, EOS signal  
✅ **Receive status updates** during processing  
✅ **Reassemble and play WAV chunks** from server  
✅ **Implement state machine** for conversation flow  

For questions or issues, refer to the server logs and use the debugging strategies outlined in Section 9.

---

**Document Version**: 1.0  
**Last Updated**: October 6, 2025  
**Server Version**: Hotpin Prototype v1.0.0  
**Author**: AI Development Team

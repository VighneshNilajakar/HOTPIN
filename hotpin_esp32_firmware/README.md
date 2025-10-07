# HotPin ESP32-CAM Firmware

## Project Status: 🚧 IN DEVELOPMENT

Firmware for ESP32-CAM AI-Thinker module with dynamic switching between camera streaming and voice interaction modes.

---

## 📋 Project Overview

This firmware implements a sophisticated dual-mode system on the ESP32-CAM platform:

### **Mode 1: Camera/Standby Mode**
- OV2640 camera streaming (VGA resolution)
- Video transmission via WebSocket
- Low-power standby state

### **Mode 2: Voice Interaction Mode**
- Full-duplex audio (INMP441 mic + MAX98357A speaker)
- Speech-to-Text (STT) via Hotpin server
- Text-to-Speech (TTS) playback
- WebSocket-based communication

---

## 🔧 Hardware Requirements

### **ESP32 Module**
- **CRITICAL**: ESP32-WROVER-E with 4MB PSRAM (standard ESP32-CAM lacks PSRAM)
- **Note**: AI-Thinker ESP32-CAM typically does NOT have PSRAM

### **Required Peripherals**
| Component | Model | Interface | Connection |
|-----------|-------|-----------|------------|
| Microphone | INMP441 | I2S | BCLK=GPIO14, WS=GPIO15, DIN=GPIO13 |
| Speaker Amp | MAX98357A | I2S | BCLK=GPIO14, WS=GPIO15, DOUT=GPIO5 |
| Push Button | Tactile | GPIO | GPIO4 (with pull-up resistor) |
| Status LED | Any LED | GPIO | GPIO2 (with current-limiting resistor) |

---

## 📌 Critical GPIO Pin Mapping

### **IMPORTANT: SD Card MUST Be Disabled**

To free up GPIOs 2, 4, 12, 13, 14, 15 for audio use, the SD card interface **must be completely disabled** in `menuconfig`:
```
Component config → SD/MMC → [ ] MMC/SDIO Host Support
```

### **Pin Assignments**

| GPIO | Function | Component | Notes |
|------|----------|-----------|-------|
| **GPIO 4** | Button Input | User input | LED disabled in firmware |
| **GPIO 2** | Status LED | System indicator | Freed from SD_D0 |
| **GPIO 5** | I2S TX Data | MAX98357A | Speaker output |
| **GPIO 13** | I2S RX Data | INMP441 | Microphone input |
| **GPIO 14** | I2S BCLK | Shared clock | TX/RX shared |
| **GPIO 15** | I2S WS | Shared clock | TX/RX shared |

### **Camera Pins (Standard AI-Thinker)**
- PWDN: GPIO32, RESET: GPIO12, XCLK: GPIO0
- D0-D7: GPIO5, GPIO18, GPIO19, GPIO21, GPIO36, GPIO39, GPIO34, GPIO35
- VSYNC: GPIO25, HREF: GPIO23, PCLK: GPIO22
- SCCB (I2C): SDA=GPIO26, SCL=GPIO27

---

## 🏗️ Architecture

### **Dual-Core Task Distribution**

**Core 0 (PRO_CPU) - I/O Operations:**
- I2S RX/TX Tasks (Priority 9)
- WebSocket Network I/O (Priority 8)
- Button FSM Handler (Priority 5)

**Core 1 (APP_CPU) - Processing:**
- System State Manager (Priority 10)
- STT Audio Processing (Priority 7)
- Camera Capture (Priority 6)
- TTS Decoder (Priority 5)

### **Memory Management**

**PSRAM Usage (CRITICAL):**
- Camera frame buffers: `MALLOC_CAP_SPIRAM`
- I2S DMA buffers: `MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA`
- STT ring buffer: 64KB in PSRAM
- TTS WAV buffer: 512KB in PSRAM

### **State Machine**

```
┌──────────────────┐
│   SYSTEM_INIT    │
└────────┬─────────┘
         ↓
┌──────────────────┐
│ CAMERA_STANDBY   │←──────┐
│  (OV2640 Active) │       │
└────────┬─────────┘       │
         │ Button          │ Button
         │ Single          │ Single
         │ Click           │ Click
         ↓                 │
┌──────────────────┐       │
│  VOICE_ACTIVE    │       │
│ (I2S Audio Mode) │───────┘
└──────────────────┘
         │
         │ Long Press (3s)
         ↓
┌──────────────────┐
│  SHUTDOWN        │
└──────────────────┘
```

---

## 🛠️ Implementation Status

### ✅ **Completed Components**

1. **config.h** - Complete hardware configuration
   - All GPIO pin definitions
   - I2S audio settings (16kHz, 16-bit, mono)
   - Task priorities and core affinity
   - PSRAM allocation macros
   - System state definitions

### 🚧 **Remaining Components (To Be Implemented)**

The following modules need to be implemented based on the architectural blueprint:

#### **1. Build System Files** (`CMakeLists.txt`)
**Location**: Root + `main/`
**Requirements:**
- ESP-IDF v4.4+ or v5.x project structure
- Link esp_camera component
- Link WebSocket client libraries
- Configure PSRAM support

#### **2. SDK Configuration** (`sdkconfig.defaults`)
**Critical Settings:**
```
CONFIG_ESP32_DEFAULT_CPU_FREQ_240=y
CONFIG_ESP32_SPIRAM_SUPPORT=y
CONFIG_SPIRAM_ALLOW_DMA=y
CONFIG_ENABLE_SDMMC_HOST=n  # CRITICAL: Disable SD card
CONFIG_CAMERA_SUPPORT=y
CONFIG_I2S_SUPPORT=y
```

#### **3. System State Manager** (`state_manager.c/h`)
**Responsibilities:**
- Finite State Machine (FSM) implementation
- I2S configuration mutex management
- Safe driver switching protocol:
  - Camera → Voice: `esp_camera_deinit()` → `i2s_driver_install()`
  - Voice → Camera: `i2s_driver_uninstall()` → `esp_camera_init()`
- Task suspension/resumption coordination

**Key Function Signatures:**
```c
void state_manager_init(void);
void state_manager_task(void *pvParameters);
esp_err_t transition_to_camera_mode(void);
esp_err_t transition_to_voice_mode(void);
```

#### **4. Button Handler** (`button_handler.c/h`)
**Responsibilities:**
- GPIO interrupt service routine (ISR)
- Software debouncing (50ms)
- Single-click vs. long-press detection (3000ms)
- FreeRTOS queue for event posting

**Key Function Signatures:**
```c
esp_err_t button_handler_init(void);
void IRAM_ATTR button_isr_handler(void *arg);
void button_fsm_task(void *pvParameters);
```

#### **5. Camera Controller** (`camera_controller.c/h`)
**Responsibilities:**
- OV2640 initialization with AI-Thinker pinout
- Frame capture task
- PSRAM-backed frame buffers
- Clean deinitialization for mode switching

**Key Function Signatures:**
```c
esp_err_t camera_init(void);
esp_err_t camera_deinit(void);
void camera_capture_task(void *pvParameters);
```

#### **6. Audio Driver Manager** (`audio_driver.c/h`)
**Responsibilities:**
- Dual I2S controller setup (I2S0 TX + I2S1 RX)
- Shared clock configuration (BCLK, WS)
- PSRAM-backed DMA buffers
- Clean uninstallation for mode switching

**Key Function Signatures:**
```c
esp_err_t audio_driver_init(void);
esp_err_t audio_driver_deinit(void);
void i2s_tx_task(void *pvParameters);  // Speaker output
void i2s_rx_task(void *pvParameters);  // Microphone input
```

#### **7. WebSocket Client** (`websocket_client.c/h`)
**Responsibilities:**
- Connection to Hotpin server (`ws://SERVER_IP:8000/ws`)
- Handshake: `{"session_id": "esp32-cam-hotpin-001"}`
- Binary PCM audio transmission (STT)
- Binary WAV audio reception (TTS)
- JSON status message handling

**Key Function Signatures:**
```c
esp_err_t websocket_init(const char *uri);
esp_err_t websocket_send_handshake(void);
esp_err_t websocket_send_audio_chunk(uint8_t *data, size_t len);
esp_err_t websocket_send_eos_signal(void);
void websocket_task(void *pvParameters);
```

#### **8. STT Pipeline** (`stt_pipeline.c/h`)
**Responsibilities:**
- Collect PCM audio from I2S RX
- Accumulate in PSRAM ring buffer (64KB)
- Stream to WebSocket on EOS signal
- 16kHz, 16-bit, mono format (Vosk compatible)

**Key Function Signatures:**
```c
esp_err_t stt_pipeline_init(void);
esp_err_t stt_start_recording(void);
esp_err_t stt_stop_recording(void);
void stt_processing_task(void *pvParameters);
```

#### **9. TTS Decoder** (`tts_decoder.c/h`)
**Responsibilities:**
- Receive WAV audio from WebSocket
- Parse 44-byte RIFF header
- Extract sample rate, channels, bit depth
- Stream PCM data to I2S TX DMA buffers

**Key Function Signatures:**
```c
esp_err_t tts_decoder_init(void);
esp_err_t tts_parse_wav_header(uint8_t *header, wav_info_t *info);
esp_err_t tts_queue_audio_chunk(uint8_t *data, size_t len);
void tts_decoder_task(void *pvParameters);
```

#### **10. Main Application** (`main.c`)
**Responsibilities:**
- System initialization sequence:
  1. Disable brownout detector
  2. Initialize NVS flash
  3. **CRITICAL**: Configure GPIO4 LOW + `rtc_gpio_hold_en(GPIO_NUM_4)`
  4. Initialize WiFi
  5. Create I2S configuration mutex
  6. Start all FreeRTOS tasks

**Required `app_main()` Sequence:**
```c
void app_main(void) {
    // 1. Disable brownout
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
    
    // 2. GPIO 4 LED control (CRITICAL)
    gpio_set_direction(GPIO_NUM_4, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_4, 0);  // LED OFF
    rtc_gpio_hold_en(GPIO_NUM_4);   // Hold state across reboots
    
    // 3. Initialize NVS, WiFi, WebSocket
    // 4. Create mutexes and queues
    // 5. Spawn tasks with proper priorities/affinity
    // 6. Enter FreeRTOS scheduler
}
```

---

## 📦 Required ESP-IDF Components

Add to `main/CMakeLists.txt`:
```cmake
idf_component_register(
    SRCS "main.c"
         "state_manager.c"
         "button_handler.c"
         "camera_controller.c"
         "audio_driver.c"
         "websocket_client.c"
         "stt_pipeline.c"
         "tts_decoder.c"
    INCLUDE_DIRS "include"
    REQUIRES 
        driver
        esp_camera
        esp_http_client
        esp_websocket_client
        nvs_flash
        esp_wifi
        freertos
        esp_timer
)
```

---

## ⚙️ Build Instructions

### **Prerequisites**
1. ESP-IDF v4.4 or v5.x installed
2. ESP32-WROVER-E module (with PSRAM)
3. USB-to-Serial adapter for flashing

### **Configuration**
```bash
cd hotpin_esp32_firmware
idf.py menuconfig
```

**Critical Settings to Verify:**
- `Component config → ESP PSRAM → [x] Support for external RAM`
- `Component config → ESP PSRAM → [x] Allow DMA access to external RAM`
- `Component config → SD/MMC → [ ] MMC/SDIO Host Support` (DISABLED)
- `Component config → Camera → AI-Thinker pin configuration`

### **Build & Flash**
```bash
idf.py build
idf.py -p COM3 flash monitor  # Adjust COM port
```

---

## 🧪 Testing Procedure

### **1. Power-On Test**
- **Expected**: Status LED (GPIO2) turns ON
- **Expected**: Flash LED (GPIO4) remains OFF
- **Expected**: Serial log shows "Groq AsyncClient initialized"

### **2. Camera Mode Test**
- **Expected**: Camera initializes successfully
- **Expected**: Video stream available via WebSocket
- **Check**: Monitor serial log for frame capture messages

### **3. Button Single-Click Test**
- **Action**: Press button once
- **Expected**: Camera stops, I2S audio drivers install
- **Expected**: Microphone starts recording
- **Expected**: Second click returns to camera mode

### **4. Button Long-Press Test**
- **Action**: Hold button for 3+ seconds
- **Expected**: System enters shutdown sequence
- **Expected**: All drivers cleanly deinitialized

---

## 🐛 Known Issues & Constraints

### **Hardware Limitations**
1. **PSRAM Mandatory**: Standard ESP32-CAM modules often lack PSRAM
   - **Solution**: Use ESP32-WROVER-E based modules only
   
2. **GPIO 4 "Ghost Flash"**: LED may flicker during boot
   - **Solution**: Implemented `rtc_gpio_hold_en()` in firmware

3. **I2S Peripheral Conflict**: Camera and audio cannot run simultaneously
   - **Solution**: Mutex-protected driver switching protocol

### **Memory Constraints**
- Total PSRAM required: ~600KB minimum
- Camera frame buffers: 2× ~100KB
- Audio buffers: ~600KB

---

## 📚 Reference Documents

- **Architecture Document**: `ESP32-CAM AI Agent Codebase Prompt.txt`
- **WebSocket Spec**: `HOTPIN_WEBSOCKET_SPECIFICATION.md`
- **ESP-IDF Docs**: https://docs.espressif.com/projects/esp-idf/

---

## 🔒 Security Notes

- WiFi credentials in `config.h` (change before deployment)
- WebSocket uses unencrypted `ws://` protocol
- For production: Implement WSS with TLS certificates

---

## 👥 Development Team

This firmware is part of the HotPin wearable AI assistant project.

**Project Structure:**
```
ESP_Warp/
├── hotpin_server/          # FastAPI WebSocket server (Python)
├── hotpin_esp32_firmware/  # ESP32-CAM firmware (C) ← YOU ARE HERE
└── model/                  # Vosk STT model
```

---

## 📝 License

Educational/Research project - 6th Semester College Project

---

## ⚠️ IMPORTANT REMINDERS

1. ✅ **PSRAM is MANDATORY** - verify with `idf.py menuconfig`
2. ✅ **SD Card MUST be disabled** - frees critical GPIOs
3. ✅ **GPIO 4 LED control** - prevents flash LED artifacts
4. ✅ **Mutex-protected driver switching** - prevents LoadProhibited exceptions
5. ✅ **Task priorities** - I2S audio highest (Priority 9)

---

**Last Updated**: October 6, 2025  
**Firmware Version**: 1.0.0-dev  
**ESP-IDF Target**: v4.4+ / v5.x

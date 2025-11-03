# ESP32 Firmware Critical Fixes Required

## Issue Summary

The WebSocket connection consistently fails during voice mode audio streaming due to **send buffer overflow** and insufficient **connection stabilization**. The ESP32 client begins streaming audio too aggressively immediately after WebSocket connection establishment, overwhelming the transport layer.

## Root Cause Analysis

### From Serial Monitor Logs:
```
E (22509) transport_ws: Error transport_poll_write
E (22510) websocket_client: esp_transport_write() returned 0
W (22594) WEBSOCKET: WebSocket send buffer full (0 bytes sent)
E (22642) websocket_client: esp_websocket_client_abort_connection(239): Websocket already stop
```

### Timeline of Failure:
1. **T+19.9s**: Voice mode transition complete, STT pipeline starts
2. **T+20.2s**: WebSocket transport verified healthy
3. **T+20.2s**: Audio streaming begins
4. **T+22.5s** (2.3s later): `transport_poll_write` error → **immediate disconnect**

### Server-Side Evidence:
```
Session initialized: esp32-cam-hotpin
🔊 [esp32-cam-hotpin] Audio chunk 1: 16 bytes
🔊 [esp32-cam-hotpin] Audio chunk 2: 4096 bytes
🔊 [esp32-cam-hotpin] Audio chunk 3: 4096 bytes
INFO:     connection closed (code=1006)  ← Abnormal closure
```

**Only 2-3 audio chunks received before catastrophic failure** - indicates ESP32 is flooding the connection faster than server can acknowledge.

---

## Required Firmware Changes

### Priority 1: Connection Stabilization Delay (CRITICAL)

**Current Issue**: 500ms delay is insufficient for WebSocket to be ready for burst traffic

**Location**: `hotpin_esp32_firmware/main/stt_pipeline.c` (or equivalent)

**Fix**:
```c
// BEFORE (lines ~21560-21562 in logs)
STT: Connection stabilization delay (500ms)...

// AFTER - Increase to 2 seconds
STT: Connection stabilization delay (2000ms)...
vTaskDelay(pdMS_TO_TICKS(2000));  // Was: 500
```

**Rationale**: WebSocket transport layer needs time to:
- Complete TCP handshake fully
- Allocate send/receive buffers
- Establish flow control state
- Server needs time to set up session handlers

---

### Priority 2: Implement Backpressure Handling

**Current Issue**: ESP32 sends audio chunks without waiting for server acknowledgments

**Location**: STT audio streaming task

**Fix**: Add acknowledgment-based flow control

```c
// Add to STT streaming task
typedef struct {
    bool waiting_for_ack;
    uint32_t last_ack_chunk;
    uint32_t chunks_sent;
    TickType_t last_ack_time;
} flow_control_t;

static flow_control_t flow_ctrl = {
    .waiting_for_ack = false,
    .last_ack_chunk = 0,
    .chunks_sent = 0
};

// In audio streaming loop
esp_err_t send_audio_chunk(const uint8_t *data, size_t len) {
    // Check if we need to wait for acknowledgment
    if ((flow_ctrl.chunks_sent - flow_ctrl.last_ack_chunk) >= 2) {
        ESP_LOGW(TAG, "Waiting for server acknowledgment (sent: %d, acked: %d)",
                 flow_ctrl.chunks_sent, flow_ctrl.last_ack_chunk);
        
        // Wait up to 500ms for ACK
        TickType_t wait_start = xTaskGetTickCount();
        while ((flow_ctrl.chunks_sent - flow_ctrl.last_ack_chunk) >= 2) {
            vTaskDelay(pdMS_TO_TICKS(10));
            
            if ((xTaskGetTickCount() - wait_start) > pdMS_TO_TICKS(500)) {
                ESP_LOGE(TAG, "ACK timeout - connection may be stalled");
                return ESP_ERR_TIMEOUT;
            }
        }
    }
    
    // Send chunk
    esp_err_t ret = esp_websocket_client_send_bin(...);
    if (ret == ESP_OK) {
        flow_ctrl.chunks_sent++;
    }
    return ret;
}

// In WebSocket event handler - text message received
if (strcmp(status, "receiving") == 0) {
    cJSON *chunks_received = cJSON_GetObjectItem(root, "chunks_received");
    if (chunks_received) {
        flow_ctrl.last_ack_chunk = chunks_received->valueint;
        flow_ctrl.last_ack_time = xTaskGetTickCount();
        ESP_LOGI(TAG, "Server ACK: %d chunks processed", flow_ctrl.last_ack_chunk);
    }
}
```

---

### Priority 3: Add Chunk Send Delay

**Current Issue**: Chunks sent too fast in tight loop

**Location**: STT audio capture task

**Fix**: Add small delay between chunks to prevent burst flooding

```c
// In audio streaming loop (after successful send)
esp_err_t ret = esp_websocket_client_send_bin(client, (const char*)chunk_buffer, bytes_read, portMAX_DELAY);

if (ret == ESP_OK) {
    chunks_sent++;
    
    // CRITICAL: Add 20ms delay between chunks to prevent send buffer saturation
    // At 16kHz, 1024-byte chunks = 32ms of audio, so 20ms delay keeps real-time
    vTaskDelay(pdMS_TO_TICKS(20));
    
    ESP_LOGD(TAG, "Audio chunk %d sent (%d bytes), paused 20ms", chunks_sent, bytes_read);
} else {
    ESP_LOGE(TAG, "Failed to send chunk %d: %s", chunks_sent, esp_err_to_name(ret));
    // Abort streaming on send failure
    break;
}
```

---

### Priority 4: Improve WebSocket Error Handling

**Current Issue**: Errors cascade without proper recovery

**Location**: WebSocket event handler

**Fix**:
```c
case WEBSOCKET_EVENT_ERROR:
    ESP_LOGE(TAG, "WebSocket error occurred");
    
    // Don't immediately reconnect - could be transport issue
    // Stop audio streaming gracefully
    if (audio_streaming_active) {
        ESP_LOGW(TAG, "Aborting audio stream due to WebSocket error");
        stt_stop_streaming();
    }
    
    // Signal state manager to transition back to camera mode
    xEventGroupSetBits(state_event_group, WS_ERROR_BIT);
    
    // Wait before reconnect attempt
    vTaskDelay(pdMS_TO_TICKS(2000));
    break;

case WEBSOCKET_EVENT_DISCONNECTED:
    ESP_LOGW(TAG, "WebSocket disconnected");
    
    // Clear flow control state
    memset(&flow_ctrl, 0, sizeof(flow_ctrl));
    
    // Stop audio streaming immediately
    if (audio_streaming_active) {
        stt_stop_streaming();
    }
    break;
```

---

### Priority 5: Fix Watchdog Task Registration

**Current Issue**: TTS playback task shows `task_wdt: esp_task_wdt_reset(705): task not found`

**Location**: TTS playback task lifecycle

**Problem**: Race condition between task creation and watchdog registration

**Fix**:
```c
// In TTS playback task creation
static void tts_playback_task(void *arg) {
    // CRITICAL: Register with watchdog FIRST before any operations
    esp_task_wdt_add(NULL);
    ESP_LOGI(TAG, "TTS playback task registered with watchdog");
    
    // ... rest of task code ...
    
    // Before task exit
    esp_task_wdt_delete(NULL);
    ESP_LOGI(TAG, "TTS playback task unregistered from watchdog");
    vTaskDelete(NULL);
}

// In task cleanup during mode transition
void tts_stop_playback(void) {
    if (tts_task_handle != NULL) {
        // Signal task to stop
        is_running = false;
        
        // Wait for task to clean itself up (max 5 seconds)
        TickType_t wait_start = xTaskGetTickCount();
        while (tts_task_handle != NULL && 
               (xTaskGetTickCount() - wait_start) < pdMS_TO_TICKS(5000)) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        
        // If task still exists, force cleanup
        if (tts_task_handle != NULL) {
            ESP_LOGW(TAG, "Force deleting TTS task - may cause watchdog error");
            esp_task_wdt_delete(tts_task_handle);  // Unregister before delete
            vTaskDelete(tts_task_handle);
            tts_task_handle = NULL;
        }
    }
}
```

---

## Server-Side Fixes Implemented ✅

### 1. Added Inactivity Timeout
- 30-second timeout for audio streaming phase
- Auto-processes buffered audio if EOS not received
- Prevents hung sessions

### 2. Improved Flow Control
- Acknowledgments sent every 2 chunks (was 5)
- Backpressure signals for ESP32
- Reduced ACK logging to avoid spam

### 3. Connection Health Checks
- Timeout on `websocket.receive()` calls
- Graceful handling of idle connections
- Better error propagation

---

## Testing Procedure

### 1. Verify Firmware Changes
```bash
cd hotpin_esp32_firmware
# Apply changes to STT and WebSocket modules
idf.py build
idf.py -p COM7 flash
idf.py monitor
```

### 2. Test Audio Streaming
1. Start server: `python main.py`
2. ESP32 boots → Camera mode
3. Single click → Voice mode transition
4. **Observe**: Connection should stabilize for 2 seconds before streaming
5. **Observe**: Serial logs should show ACK reception every 2 chunks
6. **Expected**: Audio streams for 10+ seconds without disconnect

### 3. Monitor Logs
**ESP32 Serial Monitor - Success Indicators**:
```
STT: Connection stabilization delay (2000ms)...
✅ WebSocket transport verified healthy
STT: Starting audio streaming to server...
Server ACK: 2 chunks processed
Server ACK: 4 chunks processed
[... continues without error ...]
```

**Server Logs - Success Indicators**:
```
🔊 [esp32-cam-hotpin] Audio chunk 1: 16 bytes
🔊 [esp32-cam-hotpin] Audio chunk 2: 4096 bytes
✓ [esp32-cam-hotpin] Sent acknowledgment at chunk 2
🔊 [esp32-cam-hotpin] Audio chunk 3: 4096 bytes
[... continues to receive 50+ chunks ...]
🎤 [esp32-cam-hotpin] End-of-speech signal received
```

### 4. Failure Scenarios to Eliminate
- ❌ `transport_ws: Error transport_poll_write` within 5 seconds
- ❌ `WebSocket send buffer full` messages
- ❌ Connection drops at chunk 2-3 (code 1006)
- ❌ `task_wdt: task not found` errors

---

## Additional Recommendations

### 1. WebSocket Configuration Tuning

In `main/websocket_client.c`:
```c
esp_websocket_client_config_t ws_cfg = {
    .uri = SERVER_URI,
    .buffer_size = 8192,        // Increase from default 1024
    .task_stack = 8192,         // Increase stack for heavy operations
    .ping_interval_sec = 10,    // Send keepalive pings
    .disable_auto_reconnect = true,  // Manual reconnect with backoff
};
```

### 2. Add Network Quality Monitoring
```c
// Before starting audio stream
wifi_ap_record_t ap_info;
esp_wifi_sta_get_ap_info(&ap_info);
ESP_LOGI(TAG, "WiFi RSSI: %d dBm", ap_info.rssi);

if (ap_info.rssi < -70) {
    ESP_LOGW(TAG, "Weak WiFi signal - audio streaming may be unstable");
}
```

### 3. Implement Exponential Backoff for Reconnection
```c
static uint32_t reconnect_attempt = 0;
static const uint32_t backoff_delays[] = {1000, 2000, 5000, 10000, 30000};

void websocket_reconnect(void) {
    uint32_t delay_index = (reconnect_attempt < 5) ? reconnect_attempt : 4;
    uint32_t delay_ms = backoff_delays[delay_index];
    
    ESP_LOGI(TAG, "Reconnecting in %dms (attempt %d)", delay_ms, reconnect_attempt + 1);
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    
    reconnect_attempt++;
    // Reset counter on successful connection
}
```

---

## Summary of Changes

| Component | File | Change | Priority | Status |
|-----------|------|--------|----------|--------|
| **Server** | `main.py` | Add inactivity timeout | P1 | ✅ Implemented |
| **Server** | `main.py` | Improve ACK frequency (5→2) | P1 | ✅ Implemented |
| **ESP32** | `stt_pipeline.c` | Increase stabilization (500→2000ms) | P1 | ⏳ Required |
| **ESP32** | `stt_pipeline.c` | Add backpressure handling | P1 | ⏳ Required |
| **ESP32** | `stt_pipeline.c` | Add 20ms chunk send delay | P2 | ⏳ Required |
| **ESP32** | `websocket_client.c` | Improve error handling | P2 | ⏳ Required |
| **ESP32** | `tts_decoder.c` | Fix watchdog registration | P3 | ⏳ Required |

---

## Expected Improvements

### Before Fixes:
- ❌ Connection fails after 2-3 chunks (2.3 seconds)
- ❌ Consistent `code 1006` abnormal closures
- ❌ 100% failure rate on voice mode usage
- ❌ Watchdog errors every mode transition

### After Fixes:
- ✅ Sustained streaming for 30+ seconds
- ✅ Graceful EOS signal transmission
- ✅ Proper backpressure flow control
- ✅ Clean mode transitions without watchdog errors
- ✅ Server successfully processes audio → STT → LLM → TTS pipeline

---

## Next Steps

1. **Apply ESP32 firmware changes** (Priority 1-2 changes are critical)
2. **Test with current server** (already has fixes)
3. **Monitor for 10+ voice interactions** to confirm stability
4. **Tune delays** if issues persist (increase stabilization delay to 3s, etc.)
5. **Profile memory** to ensure no leaks during transitions

---

**Critical**: The server-side fixes are **necessary but not sufficient** - ESP32 firmware changes are **mandatory** for stable operation.

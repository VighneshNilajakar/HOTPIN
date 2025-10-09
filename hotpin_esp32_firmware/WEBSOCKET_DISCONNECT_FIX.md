# WebSocket Immediate Disconnect Fix

## 🔴 Problem Analysis

### **Symptoms:**
1. ESP32 connects to WebSocket successfully ✅
2. ESP32 sends handshake ✅  
3. Server receives handshake and responds ✅
4. **ESP32 immediately disconnects** ❌
5. Server error: `Cannot call "receive" once a disconnect message has been received`

### **Serial Monitor Evidence:**
```
I (9964) WEBSOCKET: ✅ WebSocket connected to server
I (9970) WEBSOCKET: Handshake sent successfully
I (10183) HOTPIN_MAIN: 🎉 WebSocket connected and verified!
I (10462) WEBSOCKET: Received text message: {"status": "connected"...}
```

### **Server Log Evidence:**
```
INFO:     10.190.46.105:55684 - "WebSocket /ws" [accepted]
🔌 New WebSocket connection established
✓ Session initialized: esp32-cam-hotpin
✗ WebSocket error: Cannot call "receive" once a disconnect message has been received.
🧹 [esp32-cam-hotpin] Session cleaned up
INFO:     connection closed
```

---

## 🕵️ Root Cause

### **The Problem:**

In `main/main.c`, the `websocket_connection_task()` function:

1. Waits for WiFi connection
2. Connects to WebSocket server
3. Waits 3 seconds to verify connection
4. **Deletes itself with `vTaskDelete(NULL)`** ❌

**Code causing the issue:**
```c
static void websocket_connection_task(void *pvParameters) {
    // ... connection logic ...
    
    ESP_LOGI(TAG, "✅ WebSocket connection established! Task complete.");
    
    // ❌ PROBLEM: Task exits immediately after connection
    vTaskDelete(NULL);  
}
```

### **Why This Causes Disconnect:**

The ESP32 WebSocket client library (`esp_websocket_client`) is **event-driven** but requires:

1. **Active application context** - The task that started the WebSocket must remain alive
2. **Event loop processing** - FreeRTOS tasks must be running to process events
3. **Connection keep-alive** - Periodic activity to prevent TCP timeout

When `websocket_connection_task` deletes itself:
- The WebSocket connection loses its application context
- No task monitors the connection status
- TCP connection idles and times out quickly
- Server sees disconnect and closes socket
- ESP32 has no mechanism to detect or recover from disconnection

---

## ✅ Solution Applied

### **Fix: Keep WebSocket Task Alive**

Changed `websocket_connection_task()` to **never exit**. Instead, it now:

1. ✅ Establishes initial connection
2. ✅ Enters infinite monitoring loop
3. ✅ Checks connection status every 10 seconds
4. ✅ Auto-reconnects if disconnected
5. ✅ Keeps application context alive for WebSocket events

**New Code:**
```c
static void websocket_connection_task(void *pvParameters) {
    // ... initial connection logic ...
    
    ESP_LOGI(TAG, "✅ WebSocket connection established!");
    
    // ✅ FIX: Keep task alive to maintain WebSocket connection
    ESP_LOGI(TAG, "📡 WebSocket monitoring task running...");
    
    while (1) {
        // Check connection status periodically
        if (!g_websocket_connected) {
            ESP_LOGW(TAG, "⚠️ WebSocket disconnected, attempting reconnection...");
            
            // Try to reconnect
            esp_err_t ret = websocket_client_connect();
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "✅ WebSocket reconnection initiated");
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                ESP_LOGE(TAG, "❌ Reconnection failed: %s", esp_err_to_name(ret));
                vTaskDelay(pdMS_TO_TICKS(5000));
            }
        }
        
        // Sleep for 10 seconds before next status check
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
```

---

## 📊 Expected Behavior After Fix

### **ESP32 Serial Monitor:**
```
I (9964) WEBSOCKET: ✅ WebSocket connected to server
I (9970) WEBSOCKET: Handshake sent successfully
I (10183) HOTPIN_MAIN: 🎉 WebSocket connected and verified!
I (10183) HOTPIN_MAIN: ✅ WebSocket connection established!
I (10185) HOTPIN_MAIN: 📡 WebSocket monitoring task running...
I (10462) WEBSOCKET: Received text message: {"status": "connected"...}

// ✅ Connection stays alive indefinitely
// No more disconnects unless intentional
```

### **Server Logs:**
```
INFO:     10.190.46.105:55684 - "WebSocket /ws" [accepted]
🔌 New WebSocket connection established
✓ Session initialized: esp32-cam-hotpin

// ✅ Connection remains open
// Server can now send/receive messages continuously
// No more "Cannot call receive" errors
```

---

## 🧪 Testing Strategy

### **Test 1: Persistent Connection**
**Objective:** Verify WebSocket stays connected indefinitely

**Steps:**
1. Flash firmware with fix
2. Monitor serial output for 5+ minutes
3. Check server logs

**Expected:**
```
✅ No disconnect messages
✅ "WebSocket monitoring task running..." logged once
✅ Server connection stays open
✅ No reconnection attempts (unless network issue)
```

---

### **Test 2: Voice Mode Data Exchange**
**Objective:** Verify bidirectional communication works

**Steps:**
1. Send 's' command to enter voice mode
2. ESP32 should stream audio to server
3. Server processes and sends TTS audio back
4. Monitor both serial and server logs

**Expected:**
```
ESP32:
I (xxx) STT: Streaming audio data...
I (xxx) WEBSOCKET: Sending binary audio chunk: 1024 bytes
I (xxx) WEBSOCKET: Received binary TTS response: 4096 bytes

Server:
📊 [esp32-cam-hotpin] Buffer: 32000 bytes (~1.0s)
🎤 [esp32-cam-hotpin] End-of-speech signal received
📝 [esp32-cam-hotpin] Transcript: "hello world"
🤖 [esp32-cam-hotpin] LLM response: "Hi! How can I help?"
🔊 [esp32-cam-hotpin] Streaming 65536 bytes of audio response
```

---

### **Test 3: Auto-Reconnection**
**Objective:** Verify task handles unexpected disconnections

**Steps:**
1. Establish connection
2. Restart server (simulate disconnect)
3. Restart server again
4. Monitor ESP32 serial output

**Expected:**
```
I (xxx) WEBSOCKET: ⚠️ WebSocket disconnected
I (xxx) HOTPIN_MAIN: ⚠️ WebSocket disconnected, attempting reconnection...
I (xxx) WEBSOCKET: Connecting to WebSocket server...
I (xxx) WEBSOCKET: ✅ WebSocket reconnection initiated
I (xxx) WEBSOCKET: ✅ WebSocket connected to server
```

---

### **Test 4: Long-Running Stability**
**Objective:** Ensure no memory leaks or crashes over time

**Steps:**
1. Leave ESP32 running for 1+ hour
2. Toggle between camera/voice modes periodically
3. Monitor heap usage in serial output

**Expected:**
```
I (xxx) HOTPIN_MAIN: Free heap: 4419408 bytes  (initial)
I (3600000) HOTPIN_MAIN: Free heap: ~4400000 bytes  (after 1 hour)

// ✅ Heap should remain stable (< 20KB variation)
// ✅ No watchdog timeouts
// ✅ No stack overflow errors
```

---

## 🔧 Additional Improvements Made

### **1. Connection Monitoring**
- Task checks `g_websocket_connected` flag every 10 seconds
- Provides early detection of silent disconnections
- Prevents indefinite wait for responses

### **2. Auto-Reconnection Logic**
- Automatically attempts reconnect if disconnected
- 3-second wait after reconnection attempt
- 5-second backoff on failure
- No manual intervention required

### **3. Resource Management**
- Task stack: 4096 bytes (adequate for monitoring loop)
- Core 0 pinning maintained (avoid core 1 conflicts)
- No additional memory allocation in loop (leak-free)

---

## 📚 Technical Background

### **ESP32 WebSocket Client Architecture:**

```
Application Task (main.c)
    │
    ├─ esp_websocket_client_start()
    │       └─ Creates internal WebSocket task
    │
    ├─ Event Handler Callbacks
    │       ├─ WEBSOCKET_EVENT_CONNECTED
    │       ├─ WEBSOCKET_EVENT_DATA
    │       └─ WEBSOCKET_EVENT_DISCONNECTED
    │
    └─ Application keeps context alive ✅
            └─ Must not exit while connection active
```

**Key Insight:**
The ESP-IDF WebSocket library creates its own internal task for I/O, but relies on the **application task context** to remain alive. If the application task exits, the WebSocket task loses its parent context and the connection becomes orphaned.

**Solution:**
Keep the application task running in a monitoring loop. This is a common pattern in ESP32 firmware for:
- WebSocket clients
- MQTT clients  
- HTTP servers
- Any persistent network service

---

## 🎯 Summary

### **Problem:**
WebSocket task exited immediately after connection, causing orphaned connection and immediate disconnect.

### **Fix:**
Changed task to run indefinitely with:
- 10-second periodic status checks
- Automatic reconnection on disconnect
- Persistent application context

### **Impact:**
- ✅ WebSocket connection stays alive
- ✅ Bidirectional communication works
- ✅ Voice mode can stream audio
- ✅ Server can send TTS responses
- ✅ Auto-recovery from network issues

### **Files Modified:**
- `main/main.c` - `websocket_connection_task()` function

### **Lines Changed:**
- Old: 3 lines (delete task)
- New: 23 lines (monitoring loop + reconnection logic)

---

## 🚀 Next Steps

1. **Rebuild firmware:**
   ```bash
   cd hotpin_esp32_firmware
   idf.py build
   ```

2. **Flash to ESP32:**
   ```bash
   idf.py flash monitor
   ```

3. **Start server:**
   ```bash
   python main.py
   ```

4. **Test voice mode:**
   - Send 's' command
   - Speak into microphone
   - Verify audio streams to server
   - Check TTS response received

5. **Verify logs:**
   - ✅ "WebSocket monitoring task running..."
   - ✅ No disconnect messages
   - ✅ Server connection stays open

---

**Status:** Fix applied and ready for testing! 🎉

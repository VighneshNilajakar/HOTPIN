# Diagnostic Guide: Partial WAV Streaming Issue

## Problem Summary

**Observation**: Server successfully streams 255,986 bytes of TTS audio, but ESP32 disconnects immediately (WebSocket code 1005).

**Serial Monitor Status**: Log cuts off after entering VOICE mode - we don't see TTS playback logs.

## Likely Root Causes

### 1. **ESP32 Crash/Reset (Most Likely)**
The serial log truncation suggests the ESP32 may be crashing when receiving/processing the TTS audio.

**Evidence**:
- Log cuts off abruptly after voice mode transition
- No TTS decoder logs visible
- WebSocket disconnect code 1005 (abnormal closure)

**Possible Causes**:
- Memory overflow when buffering 256KB of audio
- Stack overflow in TTS playback task
- Watchdog timeout during audio processing
- PSRAM access issue

### 2. **TTS Decoder Not Starting**
The TTS decoder may not be starting when audio arrives.

**Evidence to Check**:
- Look for "TTS decoder already running" warnings
- Check for "Failed to start TTS decoder" errors

### 3. **Audio Buffer Overflow**
The 196KB TTS stream buffer may be too small for 256KB audio.

**Current Buffer Size**: 196,608 bytes (192KB)
**Audio Size**: 255,986 bytes (250KB)

**Result**: Buffer overflow, data loss, potential crash

## Diagnostic Steps

### Step 1: Capture Complete Serial Logs

The current log is truncated. We need to see what happens after entering voice mode.

**Method 1: Increase ESP-IDF Monitor Buffer**
```powershell
# Run monitor with larger buffer
idf.py monitor | Tee-Object -FilePath "full_serial_log.txt"
```

**Method 2: Use PuTTY/TeraTerm**
- Configure logging to file
- Perform test conversation
- Check for crash/reset messages

**What to Look For**:
```
E (xxxxx) task_wdt: Task watchdog got triggered
Guru Meditation Error: Core X panic'ed (...)
assert failed: ... 
E (xxxxx) TTS: Failed to ...
I (xxxxx) TTS: Received binary audio data: ...
```

### Step 2: Check for Crash Backtrace

If the ESP32 crashes, it will print a backtrace before resetting.

**Look for**:
```
Guru Meditation Error: Core 0 panic'ed (LoadProhibited)
Backtrace: 0x... 0x... 0x...
```

Run `addr2line` to decode the crash location:
```powershell
xtensa-esp32-elf-addr2line -e build/hotpin_esp32_firmware.elf 0xaddress
```

### Step 3: Increase TTS Buffer Size

The current buffer (192KB) is **smaller** than the audio (250KB). This **will cause issues**.

**File to Edit**: `hotpin_esp32_firmware/main/tts_decoder.c`

**Change Line 49**:
```c
// BEFORE:
#define TTS_STREAM_BUFFER_SIZE  (196608)  // 192 KB

// AFTER:
#define TTS_STREAM_BUFFER_SIZE  (327680)  // 320 KB (leaves safety margin)
```

**Rationale**: 320KB provides room for the largest expected TTS response plus overhead.

### Step 4: Add Defensive Logging

Add explicit logging to see where the crash occurs.

**File to Edit**: `hotpin_esp32_firmware/main/tts_decoder.c`

**In `tts_audio_callback()` function** (around line 401):
```c
static void tts_audio_callback(const uint8_t *data, size_t len, void *user_data) {
    ESP_LOGI(TAG, "📥 Audio callback: %zu bytes (buffer available: %zu)", 
             len, xStreamBufferSpacesAvailable(g_audio_stream_buffer));
    
    // Check buffer capacity BEFORE writing
    size_t space = xStreamBufferSpacesAvailable(g_audio_stream_buffer);
    if (len > space) {
        ESP_LOGE(TAG, "⚠️ BUFFER OVERFLOW WARNING: Need %zu bytes, only %zu available", len, space);
        ESP_LOGE(TAG, "   This will cause data loss and potential crash!");
    }
    
    // ... rest of function ...
}
```

### Step 5: Monitor Memory During Streaming

Enable verbose memory logging to see if PSRAM is exhausted.

**Check**: Do you see memory warnings before the crash?
```
W (xxxxx) MEM_MGR: ⚠️ WARNING: PSRAM low ...
```

## Quick Fixes to Try

### Fix #1: Increase TTS Buffer Size ✅ **DO THIS FIRST**

```c
// tts_decoder.c, line 49
#define TTS_STREAM_BUFFER_SIZE  (327680)  // 320 KB
```

Rebuild and test:
```powershell
cd f:\Documents\HOTPIN\HOTPIN\hotpin_esp32_firmware
idf.py build
idf.py -p COM7 flash monitor
```

### Fix #2: Add Stream Buffer Overflow Protection

**File**: `hotpin_esp32_firmware/main/websocket_client.c`

**In `handle_binary_message()` around line 1040**:

```c
// Before calling audio callback, check if buffer has space
if (g_audio_callback) {
    // Get buffer info from TTS decoder
    if (tts_decoder_is_running()) {
        size_t buffer_space = tts_decoder_get_buffer_space();
        if (len > buffer_space) {
            ESP_LOGW(TAG, "⚠️ Buffer full: incoming %zu bytes, space %zu bytes", len, buffer_space);
            ESP_LOGW(TAG, "   Applying backpressure - waiting for buffer to drain...");
            vTaskDelay(pdMS_TO_TICKS(50));  // Give playback task time to drain buffer
        }
    }
    
    g_audio_callback(data, len, g_audio_callback_arg);
} else {
    ESP_LOGW(TAG, "No audio callback - data discarded");
}
```

**Note**: This requires adding `tts_decoder_get_buffer_space()` function to `tts_decoder.c`.

### Fix #3: Increase Watchdog Timeout for TTS Task

If the crash is a watchdog timeout, increase the timeout.

**File**: `hotpin_esp32_firmware/sdkconfig`

Look for:
```
CONFIG_ESP_TASK_WDT_TIMEOUT_S=5
```

Change to:
```
CONFIG_ESP_TASK_WDT_TIMEOUT_S=10
```

## Testing Protocol

### Test 1: With Increased Buffer Size
1. Apply Fix #1 (increase buffer to 320KB)
2. Flash firmware
3. Run monitor with logging: `idf.py monitor | Tee-Object -FilePath "test1_log.txt"`
4. Perform voice interaction
5. Check `test1_log.txt` for complete logs

**Expected**: Should see TTS playback logs, no crash

### Test 2: With Multiple Conversations
1. After Test 1 succeeds
2. Perform 3 consecutive voice interactions
3. Check for memory leaks or buffer issues

**Expected**: All conversations should complete successfully

### Test 3: With Long Response
1. Ask a question that generates a long response (>300 words)
2. Monitor buffer usage logs
3. Check if buffer overflow occurs

**Expected**: Buffer should handle it, or log clear warning

## Success Criteria

✅ Serial monitor shows complete TTS playback logs  
✅ No "buffer overflow" warnings  
✅ No crashes or resets  
✅ Audio plays back successfully  
✅ Multiple consecutive conversations work  
✅ WebSocket stays connected throughout playback  

## Next Steps Based on Results

### If Test 1 PASSES:
- ✅ Buffer size was the issue
- Continue to Test 2 and Test 3
- Consider making buffer size configurable

### If Test 1 FAILS (still crashes):
- 📋 Analyze crash backtrace
- 🔍 Check for watchdog timeout
- 🔍 Check for memory corruption
- 🔍 Verify PSRAM is not exhausted

### If "Buffer Overflow" Warning Appears:
- Increase buffer size further (e.g., 512KB)
- Implement backpressure mechanism (Fix #2)
- Consider streaming playback (play while receiving)

## Additional Diagnostic Commands

### Check Current Buffer Configuration:
```powershell
cd f:\Documents\HOTPIN\HOTPIN\hotpin_esp32_firmware
grep -n "TTS_STREAM_BUFFER_SIZE" main/tts_decoder.c
```

### Check Memory Configuration:
```powershell
grep -n "CONFIG_ESP.*PSRAM" sdkconfig
grep -n "CONFIG_SPIRAM_CACHE_WORKAROUND" sdkconfig
```

### Monitor Heap During Runtime:
```c
// Add to tts_audio_callback()
ESP_LOGI(TAG, "Free heap: %u, Free PSRAM: %u", 
         (unsigned)esp_get_free_heap_size(),
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
```

---

## Immediate Action Required

**🚨 CRITICAL: Increase TTS buffer size immediately**

The current buffer (192KB) **cannot hold** the 256KB audio response. This is guaranteed to cause problems.

```powershell
# 1. Edit the file
code hotpin_esp32_firmware/main/tts_decoder.c

# 2. Change line 49 to:
#define TTS_STREAM_BUFFER_SIZE  (327680)  // 320 KB

# 3. Rebuild
cd hotpin_esp32_firmware
idf.py build

# 4. Flash and capture FULL logs
idf.py -p COM7 flash monitor | Tee-Object -FilePath "full_diagnostic_log.txt"

# 5. Perform test conversation

# 6. Share the full_diagnostic_log.txt
```

This will either:
- ✅ **Fix the issue** (if buffer overflow was the root cause)
- 📋 **Provide complete crash logs** (if there's a deeper issue)

Either way, we'll have the information needed to proceed.

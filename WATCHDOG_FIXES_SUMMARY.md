# ESP32 Task Watchdog Fix Summary

## Issues Identified

### Critical Issues Fixed:
1. **Task Watchdog Registration Errors** - Multiple tasks calling `esp_task_wdt_reset()` without being registered
2. **Watchdog Initialization Conflict** - Attempting to deinit/reinit watchdog causing `ESP_ERR_INVALID_STATE`

## Changes Implemented

### 1. main.c - Fixed Watchdog Initialization Strategy

**Problem:** Code tried to `esp_task_wdt_deinit()` then `esp_task_wdt_init()`, which failed because:
- The watchdog is automatically initialized by ESP-IDF startup code
- Attempting to deinit caused `ESP_ERR_INVALID_STATE` 
- Attempting to init again also failed because it was already initialized

**Solution:** Removed deinit/init logic and simply register tasks with the existing watchdog:
```c
// Old (problematic):
esp_task_wdt_deinit();  // Fails with ESP_ERR_INVALID_STATE
esp_task_wdt_init(&wdt_config);  // Also fails

// New (correct):
// Just register tasks to the existing watchdog
esp_task_wdt_add(g_state_manager_task_handle);
esp_task_wdt_add(g_websocket_task_handle);
```

**Lines Changed:** main.c:254-287

### 2. main.c - Added Watchdog Reset to WebSocket Connection Task

**Problem:** WebSocket connection task runs in long loops without resetting watchdog

**Solution:** Added watchdog reset call in the main monitoring loop:
```c
while (websocket_client_is_connected() && ...) {
    // ... checks ...
    esp_task_wdt_reset();  // Added
    vTaskDelay(pdMS_TO_TICKS(2000));
}
```

**Lines Changed:** main.c:556

### 3. tts_decoder.c - Register TTS Playback Task with Watchdog

**Problem:** TTS playback task called `safe_task_wdt_reset()` but was never registered with watchdog

**Solution:** 
- Added watchdog registration after task creation
- Added proper unregistration before task deletion
- Added cleanup in stop function

```c
// After xTaskCreatePinnedToCore:
esp_err_t wdt_ret = esp_task_wdt_add(g_playback_task_handle);

// Before vTaskDelete(NULL) in task:
esp_task_wdt_delete(NULL);

// In stop function before force delete:
esp_task_wdt_delete(g_playback_task_handle);
```

**Lines Changed:** 
- tts_decoder.c:234-238 (registration after creation)
- tts_decoder.c:323-328 (unregistration in stop)
- tts_decoder.c:658-663 (unregistration before exit)

### 4. websocket_client.c - Already Properly Implemented

**Status:** No changes needed - health check task already has proper registration/unregistration:
- Registered with watchdog at line 289
- Unregistered at line 412 before task deletion
- Uses `safe_task_wdt_reset()` wrapper to prevent error spam

## Expected Results

After flashing the updated firmware:

### Before (Issues):
```
E (7718) task_wdt: esp_task_wdt_reset(705): task not found
E (7966) task_wdt: esp_task_wdt_reset(705): task not found
E (8064) task_wdt: esp_task_wdt_reset(705): task not found
[Repeated hundreds of times]
```

### After (Fixed):
- No "task not found" errors
- Clean boot sequence
- All tasks properly monitored by watchdog
- System remains stable during operation

## System Status (From Logs)

### ✅ Working Components:
- WiFi connection (10.143.111.214)
- WebSocket connection to server (10.143.111.58:8000)
- Audio streaming from ESP32 to server
- STT transcription ("hello hello hello")
- LLM response generation
- TTS synthesis (94404 bytes)
- Camera/Voice mode transitions
- Hardware stabilization between modes

### 🔧 Fixed Components:
- Task watchdog registration
- TTS playback task monitoring
- WebSocket connection task monitoring
- State manager task monitoring

## Build and Flash Instructions

To apply these fixes:

1. Open ESP-IDF PowerShell environment:
   ```powershell
   cd f:\Documents\HOTPIN\HOTPIN\hotpin_esp32_firmware
   ```

2. Build the firmware:
   ```bash
   idf.py build
   ```

3. Flash to ESP32:
   ```bash
   idf.py -p COM7 flash monitor
   ```

4. Verify the fixes:
   - Check serial monitor for absence of "task not found" errors
   - Verify smooth operation during voice/camera mode switches
   - Confirm TTS audio playback completes successfully

## Technical Details

### Task Watchdog Timer (TWDT)
- Automatically initialized by ESP-IDF during startup
- Default timeout: 5 seconds (can be configured in menuconfig)
- Tasks must call `esp_task_wdt_reset()` periodically to prevent timeout
- Tasks must be explicitly registered with `esp_task_wdt_add()` before resetting

### Safe Wrapper Function
All tasks use a safe wrapper to prevent error spam:
```c
static inline void safe_task_wdt_reset(void) {
    esp_err_t ret = esp_task_wdt_reset();
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "WDT reset failed: %s", esp_err_to_name(ret));
    }
}
```

This logs failures at DEBUG level instead of ERROR to avoid log spam.

## Testing Checklist

- [ ] Build firmware successfully
- [ ] Flash to ESP32-CAM
- [ ] Verify no watchdog errors in serial monitor
- [ ] Test camera capture
- [ ] Test voice mode activation
- [ ] Test STT (speak into microphone)
- [ ] Test TTS playback (hear response)
- [ ] Test mode switching (camera ↔ voice)
- [ ] Verify system stability over 5+ minutes

## Notes

- The watchdog errors were not causing system crashes, but indicated improper task management
- The fixes improve system monitoring and will help catch future issues
- All working functionality remains intact - these are preventive improvements

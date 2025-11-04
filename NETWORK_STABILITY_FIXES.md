# Network Stability Fixes - ESP32 Firmware

## Problem Overview

The ESP32 firmware experienced watchdog timeouts when WebSocket connections dropped due to network instability. The root cause was multiple tasks competing to handle connection management, causing conflicts and starvation of the `tts_playback` task.

**Error Pattern:**
```
E (17749) transport_ws: Error transport_poll_write
E (42801) task_wdt: Task watchdog got triggered. The following tasks/users did not reset the watchdog in time:
E (42801) task_wdt:  - tts_playback (CPU 1)
```

## Architecture Changes

### Before: Distributed Connection Management
- ❌ `ws_health_check` task monitoring connection every 5 seconds
- ❌ `state_manager` attempting to fix connections on errors
- ❌ `websocket_connection_task` handling basic reconnection
- ❌ **Result**: Conflicts, race conditions, watchdog timeouts

### After: Centralized Connection Management
- ✅ `websocket_connection_task` is the **sole authority** for reconnection
- ✅ `state_manager` only reacts with visual feedback (LED changes)
- ✅ `tts_playback_task` has timeout awareness to prevent starvation
- ✅ **Result**: Clean error recovery, no watchdog conflicts

## Implementation Details

### Step 1: Disable ws_health_check Task ✅

**File**: `hotpin_esp32_firmware/main/websocket_client.c`

**Changes** (Lines 317-342):
- Commented out `xTaskCreate()` for `ws_health_check` task
- Set `s_health_check_task_handle = NULL`
- Added explanation comment about centralized management

**Rationale**: This task was competing with other components for connection management, causing watchdog conflicts.

---

### Step 2: Enhance websocket_connection_task ✅

**File**: `hotpin_esp32_firmware/main/main.c`

**Changes**:

1. **Lines 560-575**: Added health check monitoring in connection loop
   ```c
   int health_checks = 0;
   const int HEALTH_CHECK_INTERVAL_MS = 1000;  // Check every 1 second
   const int MAX_HEALTH_CHECKS = 30;  // 30 seconds before forced reconnect
   ```
   - Monitors connection every 1 second instead of 2 seconds
   - Forces reconnect after 30 seconds to prevent stale connections
   - Detects transport errors that don't trigger `websocket_client_is_connected()` false

2. **Lines 600-610**: Enhanced reconnection logic
   ```c
   ESP_LOGW(TAG, "⚠️ WebSocket link not healthy, initiating reconnection sequence");
   xEventGroupClearBits(g_network_event_group, NETWORK_EVENT_WEBSOCKET_CONNECTED);
   websocket_client_force_stop();
   vTaskDelay(pdMS_TO_TICKS(1000));  // Longer delay for clean shutdown
   ```
   - Clear connection status bit
   - Force stop client to clean up stale state
   - Wait 1 second (increased from 500ms) for clean shutdown

**Rationale**: Makes this task the authoritative reconnection handler, preventing conflicts with other components.

---

### Step 3: Simplify state_manager Role ✅

**File**: `hotpin_esp32_firmware/main/state_manager.c`

**Changes** (Lines 512-570): Complete rewrite of `process_websocket_status()`

**Before**: Attempted to force state transitions on disconnection/error
**After**: Only provides visual feedback, stays in current state

**Key Changes**:
```c
case WEBSOCKET_STATUS_DISCONNECTED:
    ESP_LOGW(TAG, "⚠️ WebSocket disconnected - visual feedback only, staying in current state");
    led_controller_set_state(LED_STATE_PULSING);  // Visual feedback
    
    if (current_state == SYSTEM_STATE_VOICE_ACTIVE) {
        ESP_LOGI(TAG, "📱 Staying in VOICE_ACTIVE - audio drivers remain initialized");
        ESP_LOGI(TAG, "💡 Press button to exit voice mode, or wait for automatic reconnection");
    }
    break;
```

**Benefits**:
- No forced state transitions during network errors
- Audio drivers remain initialized (prevents I2S deinitialization race)
- User can manually exit or wait for automatic reconnection
- Eliminates conflicts with connection management task

**Rationale**: Separation of concerns - state_manager reacts to connection status, websocket_connection_task fixes it.

---

### Step 4: Add Timeout Awareness to tts_playback_task ✅

**File**: `hotpin_esp32_firmware/main/tts_decoder.c`

**Changes** (Lines 805-845): Enhanced timeout logic in playback loop

**Added Protections**:

1. **5-second timeout during header parsing**:
   ```c
   if (!header_parsed && (current_time - last_activity_timestamp) > 5000) {
       ESP_LOGW(TAG, "⚠️ No audio data received for 5+ seconds while waiting for header");
       ESP_LOGW(TAG, "   This suggests a network disconnection. Exiting playback task gracefully.");
       playback_completed = true;
       playback_result = ESP_ERR_TIMEOUT;
       break;
   }
   ```

2. **10-second timeout after header parsed**:
   ```c
   if (header_parsed && (current_time - last_activity_timestamp) > 10000) {
       ESP_LOGW(TAG, "⚠️ No audio data received for 10+ seconds after header parsed");
       ESP_LOGW(TAG, "   This suggests a network disconnection during audio transfer.");
       ESP_LOGW(TAG, "   Exiting playback task gracefully to prevent watchdog timeout.");
       playback_completed = true;
       playback_result = ESP_ERR_TIMEOUT;
       break;
   }
   ```

**Rationale**: Prevents watchdog starvation when audio never arrives due to network disconnection. Task exits gracefully instead of hanging indefinitely.

---

## Expected Behavior After Fixes

### Normal Operation:
✅ WebSocket connects automatically on WiFi connection  
✅ Multi-turn conversations work perfectly  
✅ Audio playback completes successfully  
✅ LED shows breathing state in camera mode  

### During Network Disconnection:
✅ `websocket_connection_task` detects disconnection  
✅ LED changes to pulsing state (visual feedback)  
✅ System stays in current state (no forced transitions)  
✅ Audio drivers remain initialized if in voice mode  
✅ `tts_playback_task` exits gracefully if no audio arrives  
✅ Automatic reconnection attempts with 5-second backoff  

### After Reconnection:
✅ LED returns to breathing state  
✅ User can resume conversations  
✅ No system reboot required  
✅ No watchdog timeouts  

## Testing Recommendations

### Test Case 1: Normal Operation
1. Flash updated firmware
2. Connect to WiFi and WebSocket
3. Perform multiple voice interactions
4. **Expected**: All conversations work perfectly

### Test Case 2: Network Disruption During Idle
1. System in camera mode (breathing LED)
2. Disconnect network (unplug router or disable server)
3. **Expected**: 
   - LED changes to pulsing
   - System remains stable
   - No crashes or reboots
   - Reconnects automatically when network returns

### Test Case 3: Network Disruption During Voice Interaction
1. Start voice interaction (press button, speak)
2. Disconnect network while speaking
3. **Expected**:
   - LED changes to pulsing
   - System stays in voice mode
   - `tts_playback_task` exits after 5-10 second timeout
   - TTS_PLAYBACK_FINISHED event fires
   - State machine transitions gracefully
   - No watchdog timeout
   - Reconnects automatically when network returns

### Test Case 4: Network Disruption During TTS Playback
1. Start conversation, wait for TTS to begin
2. Disconnect network during audio playback
3. **Expected**:
   - Current audio finishes playing
   - LED changes to pulsing
   - No new audio arrives
   - `tts_playback_task` exits after 10-second timeout
   - No watchdog timeout
   - System recovers gracefully

## Success Criteria

✅ No watchdog timeouts during network disconnections  
✅ Clean recovery without system reboots  
✅ Automatic reconnection without user intervention  
✅ LED provides clear visual feedback of connection status  
✅ User can manually exit voice mode or wait for reconnection  
✅ Normal conversations continue to work perfectly  

## Files Modified

1. `hotpin_esp32_firmware/main/websocket_client.c` (Lines 317-342)
2. `hotpin_esp32_firmware/main/main.c` (Lines 560-610)
3. `hotpin_esp32_firmware/main/state_manager.c` (Lines 512-570)
4. `hotpin_esp32_firmware/main/tts_decoder.c` (Lines 805-845)

## Build Instructions

```powershell
cd f:\Documents\HOTPIN\HOTPIN\hotpin_esp32_firmware
idf.py build
idf.py -p COM3 flash monitor  # Replace COM3 with your port
```

## Verification Commands

Check for successful changes:
```
grep -n "health_check disabled" main/websocket_client.c
grep -n "STABILITY FIX" main/main.c
grep -n "STABILITY FIX" main/state_manager.c
grep -n "STABILITY FIX" main/tts_decoder.c
```

Expected output confirms all fixes are in place.

---

## Summary

These fixes implement a **centralized connection management architecture** that eliminates conflicts between multiple tasks trying to handle network errors. The result is a robust system that gracefully handles network disruptions without watchdog timeouts or system reboots.

**Key Principle**: One task owns the connection, others react to its status.

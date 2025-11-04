# Network Stability Validation Checklist

## Implementation Verification ✅

All 4 steps of the network stability plan have been successfully implemented:

### ✅ Step 1: ws_health_check Task Eliminated
**File**: `hotpin_esp32_firmware/main/websocket_client.c` (Lines 317-342)
- Task creation code commented out
- Handle set to NULL
- Comment explains centralized management approach

### ✅ Step 2: websocket_connection_task as Sole Authority
**File**: `hotpin_esp32_firmware/main/main.c` (Lines 560-620)
- Simple, robust connection loop implemented
- Monitors connection every 1 second (was 2 seconds)
- Forces reconnect after 30 seconds to prevent stale connections
- 5-second backoff between reconnection attempts
- Calls `websocket_client_force_stop()` before reconnect
- Single source of truth for connection management

### ✅ Step 3: State Manager Decoupled
**File**: `hotpin_esp32_firmware/main/state_manager.c` (Lines 512-570)
- Only provides visual feedback (LED changes)
- Does NOT force state transitions
- Does NOT attempt to fix network connection
- Stays in current state during disconnections
- Audio hardware remains initialized in VOICE_ACTIVE mode
- User can manually exit or wait for automatic reconnection

### ✅ Step 4: TTS Task Timeout-Aware
**File**: `hotpin_esp32_firmware/main/tts_decoder.c` (Lines 805-845)
- Uses 100ms timeout on `xStreamBufferReceive()` (not `portMAX_DELAY`)
- Tracks `last_activity_timestamp` for timeout detection
- 5-second timeout during header parsing (early network failure detection)
- 10-second timeout after header parsed (mid-stream failure detection)
- Always calls `safe_task_wdt_reset()` in every loop iteration
- Exits gracefully with `ESP_ERR_TIMEOUT` when no audio arrives
- Prevents watchdog starvation

---

## Testing Protocol

### Test 1: Normal Multi-Turn Conversations ✅
**Objective**: Verify core functionality remains intact

**Procedure**:
1. Flash updated firmware to ESP32
2. Start Python WebSocket server
3. Perform 3-5 consecutive voice interactions
4. Observe LED patterns and serial monitor

**Expected Results**:
- ✅ All conversations complete successfully
- ✅ LED shows breathing pattern in camera mode
- ✅ LED shows solid pattern during voice recording
- ✅ Audio playback is clear and complete
- ✅ No crashes, reboots, or watchdog timeouts
- ✅ State transitions are smooth

**Status**: Ready for testing

---

### Test 2: Network Disruption During Idle ✅
**Objective**: Verify graceful handling of connection loss in standby mode

**Procedure**:
1. ESP32 in camera standby mode (breathing LED)
2. Stop Python WebSocket server (or disconnect network)
3. Wait 30 seconds
4. Restart Python WebSocket server (or reconnect network)
5. Monitor serial logs and LED behavior

**Expected Results**:
- ✅ LED changes to pulsing pattern when connection drops
- ✅ Serial log shows reconnection attempts every 5 seconds
- ✅ No crashes or reboots
- ✅ No watchdog timeouts
- ✅ ESP32 automatically reconnects within 5-10 seconds
- ✅ LED returns to breathing pattern after reconnection
- ✅ Device ready for new voice command

**Status**: Ready for testing

---

### Test 3: Network Disruption During Voice Recording ✅
**Objective**: Verify robust handling of connection loss during audio capture

**Procedure**:
1. Press button to enter voice mode (solid LED)
2. Start speaking (audio being captured and streamed)
3. Stop Python WebSocket server mid-sentence
4. Continue speaking for 2-3 seconds
5. Release button
6. Monitor behavior

**Expected Results**:
- ✅ LED changes to pulsing pattern immediately
- ✅ STT pipeline detects connection loss
- ✅ System gracefully handles incomplete audio stream
- ✅ NO crash or reboot
- ✅ NO watchdog timeout
- ✅ System stays in VOICE_ACTIVE mode (audio drivers initialized)
- ✅ User sees pulsing LED indicating "waiting for reconnection"
- ✅ Serial log shows reconnection attempts

**Restart server**:
- ✅ ESP32 reconnects automatically
- ✅ LED returns to breathing pattern
- ✅ Can immediately start new conversation

**Status**: Ready for testing

---

### Test 4: Network Disruption During TTS Playback ✅ (CRITICAL)
**Objective**: Verify tts_playback_task timeout logic prevents watchdog crash

**Procedure**:
1. Start voice conversation
2. Speak and release button
3. Wait for transcription to complete
4. Server processes LLM response and begins sending TTS audio
5. **CRITICAL**: Stop Python server AFTER TTS audio starts arriving but BEFORE it completes
6. Monitor ESP32 behavior closely

**Expected Results - Phase 1 (Immediate)**:
- ✅ Currently playing audio chunk continues to completion
- ✅ LED changes to pulsing pattern
- ✅ `tts_playback_task` continues waiting for more audio

**Expected Results - Phase 2 (10 seconds later)**:
- ✅ `tts_playback_task` timeout triggers (10-second timeout)
- ✅ Serial log shows: "⚠️ No audio data received for 10+ seconds after header parsed"
- ✅ Task exits gracefully with `ESP_ERR_TIMEOUT`
- ✅ `TTS_PLAYBACK_FINISHED` event fires
- ✅ State machine handles transition properly
- ✅ **NO watchdog timeout** (this is the key success criterion)
- ✅ NO crash or reboot

**Expected Results - Phase 3 (Reconnection)**:
- ✅ `websocket_connection_task` continues reconnection attempts
- ✅ LED remains pulsing
- ✅ System remains stable

**Restart server**:
- ✅ ESP32 reconnects automatically
- ✅ LED returns to breathing pattern
- ✅ Can start new conversation immediately

**Status**: Ready for testing (this is the most critical test)

---

### Test 5: Prolonged Network Outage ✅
**Objective**: Verify system stability during extended disconnection

**Procedure**:
1. ESP32 connected and stable
2. Disconnect network completely
3. Leave disconnected for 5 minutes
4. Monitor for any instability
5. Reconnect network

**Expected Results**:
- ✅ LED shows pulsing pattern throughout outage
- ✅ Serial log shows reconnection attempts every 5 seconds
- ✅ System remains stable for entire duration
- ✅ NO memory leaks
- ✅ NO watchdog timeouts
- ✅ NO crashes or reboots
- ✅ Automatic reconnection when network returns
- ✅ Full functionality restored immediately

**Status**: Ready for testing

---

### Test 6: Rapid Connection Cycling ✅
**Objective**: Verify robustness under rapid network instability

**Procedure**:
1. Start Python server
2. Wait for ESP32 to connect
3. Stop server
4. Wait 3 seconds
5. Start server
6. Wait 3 seconds
7. Repeat steps 3-6 five times
8. Monitor ESP32 behavior

**Expected Results**:
- ✅ LED cycles between breathing and pulsing patterns
- ✅ ESP32 handles all connection/disconnection events gracefully
- ✅ NO crashes or reboots
- ✅ NO watchdog timeouts
- ✅ System remains responsive throughout
- ✅ After final server start, ESP32 stabilizes normally

**Status**: Ready for testing

---

## Success Criteria Summary

### Critical (Must Pass):
- ✅ No watchdog timeouts during any network disruption scenario
- ✅ No system crashes or reboots during connection loss
- ✅ `tts_playback_task` exits gracefully when audio doesn't arrive (Test 4)
- ✅ Automatic reconnection without manual intervention

### Important (Should Pass):
- ✅ LED provides clear visual feedback of connection status
- ✅ System stays in current state during disconnections (no forced transitions)
- ✅ Audio hardware remains initialized in VOICE_ACTIVE mode
- ✅ Clean serial log messages explaining behavior

### Desirable (Nice to Have):
- ✅ Reconnection within 5-10 seconds of server restart
- ✅ User can manually exit voice mode during disconnection if desired
- ✅ System handles prolonged outages (5+ minutes) without degradation

---

## Build and Flash Commands

```powershell
# Navigate to firmware directory
cd f:\Documents\HOTPIN\HOTPIN\hotpin_esp32_firmware

# Clean build (recommended for major changes)
idf.py fullclean
idf.py build

# Flash and monitor
idf.py -p COM3 flash monitor  # Replace COM3 with your port

# Monitor only (after flashing)
idf.py -p COM3 monitor
```

---

## Debugging Tips

### Serial Monitor Keywords to Watch For:
- ✅ **Good signs**: "📡 WebSocket connection active", "✅ WebSocket connected"
- ⚠️ **Expected warnings**: "⚠️ WebSocket link not healthy", "⚠️ No audio data received"
- ❌ **Bad signs**: "E (xxxxx) task_wdt: Task watchdog got triggered", "assert failed"

### LED Pattern Reference:
- **Breathing**: Connected and ready (camera standby)
- **Solid**: Voice recording in progress
- **Pulsing**: Disconnected, attempting reconnection
- **SOS/Fast blink**: Error state (should not occur with these fixes)

### Common Issues and Solutions:
1. **"Failed to create WebSocket connection task"**
   - Check available heap memory
   - Reduce task stack sizes if needed

2. **"WebSocket connection failed: ESP_FAIL"**
   - Verify server is running: `python main.py`
   - Check IP address in `sdkconfig`: `10.143.111.58:8000`

3. **Continuous reconnection attempts**
   - Normal behavior when server is offline
   - Should stop and succeed when server is started

---

## Files Modified in This Fix

1. **`websocket_client.c`** (Lines 317-342)
   - Disabled `ws_health_check` task

2. **`main.c`** (Lines 560-620)
   - Enhanced `websocket_connection_task` with health monitoring
   - Centralized reconnection logic

3. **`state_manager.c`** (Lines 512-570)
   - Simplified `process_websocket_status()` to only provide visual feedback
   - Removed forced state transitions

4. **`tts_decoder.c`** (Lines 805-845)
   - Added timeout awareness to `tts_playback_task`
   - 5-second timeout during header parsing
   - 10-second timeout during audio streaming
   - Graceful exit with `ESP_ERR_TIMEOUT`

---

## Post-Testing Checklist

After running all tests, verify:

- [ ] Test 1 (Normal conversations) passed
- [ ] Test 2 (Idle disconnection) passed
- [ ] Test 3 (Recording disconnection) passed
- [ ] Test 4 (TTS playback disconnection) passed ← **Most Critical**
- [ ] Test 5 (Prolonged outage) passed
- [ ] Test 6 (Rapid cycling) passed
- [ ] No watchdog timeouts observed in any test
- [ ] No crashes or reboots in any test
- [ ] LED patterns match expected behavior
- [ ] Serial logs show clean reconnection logic
- [ ] User experience feels robust and reliable

---

## Next Steps After Validation

If all tests pass:
1. ✅ Mark firmware as stable for network disruptions
2. ✅ Update main README with network resilience features
3. ✅ Consider reducing reconnection backoff to 3 seconds for faster recovery
4. ✅ Document LED pattern behaviors for end users

If any tests fail:
1. 🔍 Analyze serial logs to identify failure point
2. 🔍 Check which task triggered watchdog (if applicable)
3. 🔍 Review timeout values (may need adjustment)
4. 🔍 Verify event flow in state machine
5. 🔍 Re-run specific failing test with increased logging

---

## Architecture Summary

**Before**: Distributed, chaotic connection management ❌
- Multiple tasks fighting for control
- Race conditions and conflicts
- Watchdog timeouts during disruptions

**After**: Centralized, robust connection management ✅
- Single source of truth (`websocket_connection_task`)
- Clean separation of concerns
- Timeout-aware tasks prevent starvation
- Graceful degradation during network issues

**Key Principle**: One task owns the connection, others react to its status.

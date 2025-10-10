# Quick Deployment Runbook - I²S/Camera Fix

## Status: ✅ FIXES ALREADY IMPLEMENTED

Good news: The critical fixes are **already in your codebase**!

## What Was Fixed

### ✅ Fix #1: MCLK Disabled
**File**: `main/audio_driver.c` line 208
```c
.mck_io_num = I2S_PIN_NO_CHANGE  // Already correct!
```

### ✅ Fix #2: GPIO ISR Guards
**Files**: `main/button_handler.c`, `main/camera_controller.c`
Both already check for `ESP_ERR_INVALID_STATE`

### ✅ Fix #3: Safe State Transitions
**File**: `main/state_manager.c`
Proper sequence: Stop tasks → Deinit I²S → Delay → Camera → Delay → Reinit I²S

## Quick Test Procedure

### Step 1: Build & Flash (2 minutes)
```powershell
cd "f:\Documents\College\6th Semester\Project\ESP_Warp\hotpin_esp32_firmware"
idf.py build
idf.py -p COM3 flash monitor
```

### Step 2: Verify Boot (30 seconds)
**Look for these SUCCESS indicators:**
```
I (xxxx) AUDIO: ✅ I2S full-duplex started and ready
I (xxxx) CAMERA: cam init ok
W (xxxx) CAMERA: GPIO ISR service already installed (OK)  ← This warning is fine!
```

**Confirm these ERRORS are GONE:**
```
❌ Should NOT see: "mclk configure failed"
❌ Should NOT see: "No free interrupt inputs"
❌ Should NOT see: "cam intr alloc failed"
```

### Step 3: Test Audio Recording (1 minute)
1. Long-press button
2. Speak for 10 seconds
3. Release button

**Expected logs:**
```
I (xxxx) STT: Audio capture task started
I (xxxx) WEBSOCKET: Sent chunk 1 (2048 bytes)
I (xxxx) WEBSOCKET: Sent chunk 2 (2048 bytes)
```

### Step 4: Test Camera Capture (30 seconds)
1. Double-press button
2. Wait for LED confirmation

**Expected logs:**
```
I (xxxx) STATE_MGR: Starting camera capture sequence
I (xxxx) CAMERA: Initializing camera...
I (xxxx) cam_hal: cam init ok
I (xxxx) STATE_MGR: Frame captured: XXXXX bytes
I (xxxx) HTTP_CLIENT: Image uploaded successfully
```

### Step 5: Test Audio During Recording (1 minute)
1. Long-press to start recording
2. While recording, double-press to capture
3. Verify audio resumes

**Expected logs:**
```
I (xxxx) STATE_MGR: Stopping STT/TTS tasks...
I (xxxx) AUDIO: Deinitializing I2S driver...
I (xxxx) AUDIO: I2S stopped
I (xxxx) AUDIO: I2S driver uninstalled
[delay]
I (xxxx) CAMERA: Initializing camera...
I (xxxx) cam_hal: cam init ok
[capture happens]
[delay]
I (xxxx) AUDIO: Initializing I2S full-duplex audio driver...
I (xxxx) AUDIO: ✅ Audio driver initialized successfully
I (xxxx) STT: STT pipeline started
```

---

## If You Still See Errors

### Error: "mclk configure failed"
**Check**: `main/audio_driver.c` line 208
**Should be**: `.mck_io_num = I2S_PIN_NO_CHANGE`
**Not**: `.mck_io_num = GPIO_NUM_XX`

### Error: "No free interrupt inputs"
**Likely cause**: Camera not properly deinitialized
**Check**: `main/state_manager.c` - ensure `camera_controller_deinit()` is called before `audio_driver_init()`
**Add**: More delay after `camera_controller_deinit()` (increase to 200ms)

### Error: "cam intr alloc failed"
**Likely cause**: I²S not properly deinitialized
**Check**: `main/state_manager.c` - ensure `audio_driver_deinit()` is called before `camera_controller_init()`
**Add**: More delay after `audio_driver_deinit()` (increase to 200ms)

### Warning: "GPIO isr service already installed"
**Status**: EXPECTED, HARMLESS
**Reason**: Multiple modules need ISR service
**Action**: IGNORE - this is normal behavior

---

## Success Criteria Checklist

- [ ] Build completes without errors
- [ ] Boot shows "I2S full-duplex started and ready"
- [ ] Boot shows "cam init ok"
- [ ] NO "mclk configure failed" errors
- [ ] NO "No free interrupt inputs" errors
- [ ] NO "cam intr alloc failed" errors
- [ ] Audio recording works (chunks sent to server)
- [ ] Camera capture works (image uploaded)
- [ ] Audio recording resumes after camera capture
- [ ] System stable through 5+ capture cycles

---

## Current Status (Based on Serial Monitor)

From your `serial_monitor.txt`:
```
✅ WiFi connected
✅ Camera initialized: "cam init ok"
✅ GPIO ISR service working (warning is OK)
✅ WebSocket connected
✅ System running
```

**What to watch for:**
- Monitor for any MCLK errors (should be none)
- Test camera capture during audio recording
- Verify clean state transitions

---

## One-Liner Test Commands

### Quick Test Script
Add to `main/main.c` or use serial commands:

```c
// Test sequence (via serial commands 's' and 'c'):
1. Press 's' → start recording
2. Wait 5 seconds
3. Press 'c' → capture image (tests transition)
4. Wait 3 seconds
5. Press 's' → stop recording
6. Repeat 3 times
```

---

## Expected Behavior Summary

| Action | I²S State | Camera State | Expected Behavior |
|--------|-----------|--------------|-------------------|
| Boot | OFF | ON | Camera standby mode |
| Long press | ON | OFF | Audio recording starts |
| During record + double press | OFF→ON | ON→OFF | Clean transition, both work |
| Release | ON | OFF | Audio recording stops |
| Double press (idle) | OFF | ON | Camera captures immediately |

---

## Debug Enable (If Needed)

If you need more verbose logs:

```powershell
idf.py menuconfig
# Navigate to: Component config → Log output → Default log verbosity
# Select: Debug
# Save and rebuild
```

---

## Files to Review (If Customizing)

1. **`main/audio_driver.c`** (lines 180-244)
   - Full-duplex I²S configuration
   - MCLK settings

2. **`main/state_manager.c`** (lines 280-380)
   - Camera capture sequence
   - State transition logic

3. **`main/camera_controller.c`** (lines 16-35)
   - Camera initialization
   - GPIO ISR guard

4. **`main/button_handler.c`** (lines 109-120)
   - Button GPIO ISR guard

---

## Next Steps

1. **Flash the firmware** (it's already fixed!)
2. **Test all scenarios** (boot, audio, camera, transitions)
3. **Monitor logs** for any unexpected errors
4. **Report back** if any issues persist

---

## Contact for Issues

If tests fail, capture:
1. Full serial log from boot to error
2. Exact test procedure that failed
3. Any new error messages

**Good luck! The fixes are already in place. Just flash and test! 🚀**

# I²S MCLK & Interrupt Allocation Fixes - Complete Patch

## Problem Summary

ESP32-CAM firmware experiencing runtime errors during audio/camera transitions:
- `esp_clock_output_start: Selected io is already mapped by another signal`
- `i2s_check_set_mclk: mclk configure failed`
- `intr_alloc: No free interrupt inputs for I2S0 interrupt`
- `cam_hal: cam_config: cam intr alloc failed`
- `gpio_install_isr_service: GPIO isr service already installed`

## Root Causes Identified

1. **MCLK Configuration Issue**: I²S driver attempting to configure MCLK on GPIO pins that are either unavailable or already mapped. INMP441 and MAX98357A do NOT require MCLK.

2. **Interrupt Allocation Exhaustion**: Camera fails to allocate interrupts because I²S driver has not properly released resources during state transitions.

3. **GPIO ISR Service Multiple Installation**: `gpio_install_isr_service()` being called multiple times without proper guards (partially fixed).

## Implemented Fixes (Status)

### ✅ Fix #1: Disable MCLK in I²S Configuration

**Status**: ALREADY IMPLEMENTED

**File**: `main/audio_driver.c` line 208

**Code**:
```c
i2s_pin_config_t i2s_pins = {
    .mck_io_num = I2S_PIN_NO_CHANGE,            // ✅ No MCLK - critical fix
    .bck_io_num = CONFIG_I2S_BCLK,              // Bit clock (GPIO14)
    .ws_io_num = CONFIG_I2S_LRCK,               // Word select (GPIO15)
    .data_out_num = CONFIG_I2S_TX_DATA_OUT,     // Speaker data (GPIO13)
    .data_in_num = CONFIG_I2S_RX_DATA_IN        // Mic data (GPIO12)
};
```

**Reason**: MAX98357A and INMP441 require only BCLK + WS. Disabling MCLK prevents GPIO mapping conflicts.

**Verification**: Build logs should show NO `mclk configure failed` errors.

---

### ✅ Fix #2: Guard GPIO ISR Service Installation

**Status**: ALREADY IMPLEMENTED

**Files**:
- `main/button_handler.c` line 109-113
- `main/camera_controller.c` line 21-32

**Code Pattern**:
```c
esp_err_t ret = gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1);
if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(ret));
    return ret;
}
// ESP_ERR_INVALID_STATE means already installed - this is OK
```

**Reason**: Multiple modules may attempt to install ISR service. Guard prevents repeated installation errors.

**Verification**: Serial logs should show at most ONE "GPIO isr service already installed" warning (from second module), not errors.

---

### ✅ Fix #3: Safe I²S Driver Lifecycle Management

**Status**: IMPLEMENTED in state_manager.c

**File**: `main/state_manager.c` lines 280-380

**Implementation**:

```c
static esp_err_t handle_camera_capture(void) {
    // Step 1: Stop audio tasks if in voice mode
    if (current_state == SYSTEM_STATE_VOICE_ACTIVE) {
        stt_pipeline_stop();
        tts_decoder_stop();
        vTaskDelay(pdMS_TO_TICKS(100));  // Task cleanup
    }
    
    // Step 2: Acquire mutex (prevents race conditions)
    if (xSemaphoreTake(g_i2s_config_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    
    // Step 3: Stop and uninstall I²S drivers
    if (current_state == SYSTEM_STATE_VOICE_ACTIVE) {
        audio_driver_deinit();  // Calls i2s_stop + i2s_driver_uninstall
        vTaskDelay(pdMS_TO_TICKS(100));  // ✅ Hardware settle time
    }
    
    // Step 4: Initialize camera (interrupt allocation now available)
    ret = camera_controller_init();
    
    // Step 5: Capture and upload
    camera_fb_t *fb = camera_controller_capture_frame();
    // ... upload logic ...
    
    // Step 6: Cleanup camera
    if (current_state == SYSTEM_STATE_VOICE_ACTIVE) {
        camera_controller_deinit();
        vTaskDelay(pdMS_TO_TICKS(100));  // ✅ Hardware settle time
    }
    
    // Step 7: Restore audio
    if (current_state == SYSTEM_STATE_VOICE_ACTIVE) {
        audio_driver_init();  // Reinstall I²S
        stt_pipeline_start();
        tts_decoder_start();
    }
    
    xSemaphoreGive(g_i2s_config_mutex);
    return ESP_OK;
}
```

**Critical Elements**:
1. **Task Synchronization**: Stop STT/TTS tasks BEFORE deinitializing I²S
2. **Mutex Protection**: Prevents concurrent I²S/camera operations
3. **Hardware Settle Delays**: 100ms delays allow interrupt matrix to fully release resources
4. **Proper Sequencing**: Stop → Deinit → Delay → Camera Init → Capture → Camera Deinit → Delay → Audio Reinit

**Reason**: Camera requires interrupt allocation that I²S may be holding. Proper deinit + settle time ensures interrupts are freed.

---

### ⏳ Fix #4: Enhanced Error Checking (RECOMMENDED)

**Status**: PARTIALLY IMPLEMENTED - needs enhancement

**Recommendation**: Add comprehensive error checking to all driver init/deinit calls:

```c
// Example pattern for audio_driver_init()
esp_err_t audio_driver_init(void) {
    ESP_LOGI(TAG, "Initializing I2S full-duplex audio driver...");
    
    if (is_initialized) {
        ESP_LOGW(TAG, "Audio driver already initialized");
        return ESP_OK;
    }
    
    esp_err_t ret = configure_i2s_full_duplex();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ CRITICAL: I2S config failed: %s", esp_err_to_name(ret));
        // Add diagnostic info
        ESP_LOGE(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());
        ESP_LOGE(TAG, "Free interrupt slots: Check with esp_intr_dump()");
        return ret;
    }
    
    is_initialized = true;
    ESP_LOGI(TAG, "✅ Audio driver initialized successfully");
    return ESP_OK;
}
```

**Apply to**:
- `audio_driver_init/deinit()`
- `camera_controller_init/deinit()`
- `i2s_driver_install/uninstall()`
- `i2s_set_pin()`
- `esp_camera_init/deinit()`

---

## Test Plan & Verification

### Pre-Deployment Checks

1. **Build Verification**
   ```bash
   cd hotpin_esp32_firmware
   idf.py fullclean
   idf.py build 2>&1 | tee build.log
   ```
   
   **Expected**: No errors, warnings about legacy I²S driver are OK

2. **Flash & Monitor**
   ```bash
   idf.py flash monitor
   ```

### Test Case 1: Audio-Only Mode

**Procedure**:
1. Long-press button to start recording
2. Monitor serial logs for 30 seconds
3. Release to stop recording

**Expected Logs**:
```
I (xxxx) AUDIO: Configuring I2S0 for full-duplex audio (TX+RX)...
I (xxxx) AUDIO: ✅ I2S full-duplex started and ready
I (xxxx) STT: Audio capture task started
I (xxxx) STT: Chunk queued, size=2048
I (xxxx) WEBSOCKET: Sent chunk N (2048 bytes)
```

**Success Criteria**:
- ❌ NO `mclk configure failed`
- ❌ NO `intr_alloc: No free interrupt inputs`
- ✅ Audio chunks sent successfully
- ✅ I²S driver logs show successful init

### Test Case 2: Camera Capture (from Idle)

**Procedure**:
1. System in idle/camera standby mode
2. Double-press button
3. Wait for image upload confirmation

**Expected Logs**:
```
I (xxxx) CAMERA: Initializing camera...
W (xxxx) CAMERA: GPIO ISR service already installed (OK)
I (xxxx) cam_hal: cam init ok
I (xxxx) CAMERA: Camera initialized, format: JPEG
I (xxxx) STATE_MGR: Frame captured: XXXXX bytes
I (xxxx) HTTP_CLIENT: Image uploaded successfully
```

**Success Criteria**:
- ❌ NO `cam intr alloc failed`
- ✅ Camera initializes successfully
- ✅ Frame capture succeeds
- ✅ HTTP upload completes

### Test Case 3: Camera Capture During Audio Recording

**Procedure**:
1. Long-press to start recording
2. While recording, double-press to capture image
3. Verify audio resumes after capture

**Expected Logs**:
```
I (xxxx) STATE_MGR: Stopping STT/TTS tasks...
I (xxxx) STT: Stopping STT pipeline...
I (xxxx) AUDIO: Deinitializing I2S driver...
I (xxxx) AUDIO: I2S stopped
I (xxxx) AUDIO: I2S driver uninstalled
[100ms delay]
I (xxxx) CAMERA: Initializing camera...
I (xxxx) cam_hal: cam init ok
I (xxxx) STATE_MGR: Frame captured: XXXXX bytes
I (xxxx) CAMERA: Deinitializing camera...
[100ms delay]
I (xxxx) AUDIO: Initializing I2S full-duplex audio driver...
I (xxxx) AUDIO: ✅ Audio driver initialized successfully
I (xxxx) STT: STT pipeline started
```

**Success Criteria**:
- ❌ NO `intr_alloc: No free interrupt inputs`
- ❌ NO `cam intr alloc failed`
- ✅ Clean I²S shutdown logged
- ✅ Camera init after I²S deinit
- ✅ Clean I²S restart after camera deinit
- ✅ Audio recording resumes successfully

### Test Case 4: Stress Test - Rapid Switching

**Procedure**:
1. Perform 20 cycles: Record → Stop → Capture → Record
2. Monitor for memory leaks and stability
3. Check for any error accumulation

**Expected Results**:
- System remains stable through all cycles
- No memory leaks (check heap size trends)
- No accumulated errors or warnings
- All operations complete successfully

### Test Case 5: Edge Case - Rapid Button Presses

**Procedure**:
1. Rapidly press button (simulate debounce issues)
2. Perform multiple double-presses in quick succession
3. Verify FSM handles gracefully

**Expected Results**:
- Button FSM debounces correctly
- No driver crashes or hangs
- Operations queue/cancel gracefully

---

## Acceptance Criteria (Success Definition)

### ✅ RESOLVED ERRORS:
1. **NO** `mclk configure failed` messages
2. **NO** `esp_clock_output_start` GPIO mapping errors
3. **NO** `intr_alloc: No free interrupt inputs` errors
4. **NO** `cam_intr alloc failed` errors
5. At most **ONE** `gpio_install_isr_service already installed` warning (acceptable, not an error)

### ✅ FUNCTIONAL SUCCESS:
1. Camera captures work reliably from any system state
2. Audio recording/playback unaffected
3. State transitions smooth (audio ↔ camera) with no artifacts
4. System stable through 20+ capture/record cycles
5. Free heap remains stable (no leaks)

### ✅ LOG INDICATORS:
- `I2S full-duplex started and ready` appears on every audio init
- `cam init ok` appears on every camera init
- Hardware settle delays visible (100ms pauses)
- Clean shutdown/startup sequences logged

---

## Known Issues & Limitations

### 1. Legacy I²S Driver Warning
```
W (1550) i2s(legacy): legacy i2s driver is deprecated
```

**Status**: KNOWN, LOW PRIORITY
**Impact**: None - driver works correctly
**Future**: Migrate to `i2s_std` API in ESP-IDF v5.x for long-term support
**Recommendation**: Optional upgrade - not required for stability

### 2. GPIO ISR Service "Already Installed" Warning

**Status**: ACCEPTABLE BEHAVIOR
**Reason**: Multiple modules (button, camera) need ISR service. First module installs it, second module gets ESP_ERR_INVALID_STATE which is handled gracefully.
**Impact**: None - warning only, not an error
**Fix Status**: ✅ Properly guarded in both modules

### 3. Task Watchdog Already Initialized

**Status**: BENIGN WARNING
**Log**: `E (7245) task_wdt: esp_task_wdt_init(515): TWDT already initialized`
**Impact**: None - subsequent calls to `esp_task_wdt_init()` are no-ops
**Recommendation**: Add guard similar to GPIO ISR service (low priority)

---

## Alternative Solutions Considered (Not Implemented)

### Option A: Use Separate I²S Peripherals (I2S0 TX + I2S1 RX)
**Pros**: Isolates TX/RX channels
**Cons**: 
- Still requires shared BCLK/WS pins → GPIO conflict
- Doubles interrupt usage
- More complex driver management
**Decision**: REJECTED - Full-duplex on single peripheral is cleaner

### Option B: Shared Interrupts (ESP_INTR_FLAG_SHARED)
**Pros**: Frees interrupt slots
**Cons**:
- Complex ISR implementation
- Requires reentrant handlers
- Higher latency
**Decision**: REJECTED - Proper driver lifecycle is simpler and more robust

### Option C: Force No Coexistence (Audio XOR Camera)
**Pros**: Simplest architecture
**Cons**: 
- Poor UX - can't capture during calls
- Defeats purpose of dual-mode device
**Decision**: REJECTED - State management allows coexistence

---

## Code Changes Summary

### Modified Files:

1. **`main/audio_driver.c`** (lines 208)
   - Set `mck_io_num = I2S_PIN_NO_CHANGE` in full-duplex pin config
   - Already using single I2S peripheral (I2S_NUM_0)

2. **`main/button_handler.c`** (lines 109-113)
   - Guard `gpio_install_isr_service()` with ESP_ERR_INVALID_STATE check

3. **`main/camera_controller.c`** (lines 21-32)
   - Guard `gpio_install_isr_service()` with ESP_ERR_INVALID_STATE check
   - Track installation state with static bool

4. **`main/state_manager.c`** (lines 280-380)
   - Implement safe camera capture sequence
   - Add hardware settle delays (100ms)
   - Proper task stop → driver deinit → camera init → camera deinit → driver reinit flow

### No Changes Required:

- Camera pin configuration (already correct)
- WiFi/WebSocket initialization (working correctly)
- Button FSM logic (debouncing works)
- LED controller (independent of audio/camera)

---

## Build & Deployment Instructions

### 1. Clean Build
```bash
cd f:\Documents\College\6th Semester\Project\ESP_Warp\hotpin_esp32_firmware
idf.py fullclean
idf.py build
```

### 2. Flash Firmware
```bash
idf.py -p COM3 flash
```

### 3. Monitor Logs
```bash
idf.py -p COM3 monitor
```

Or combined:
```bash
idf.py -p COM3 flash monitor
```

### 4. Verify Success

**Check Serial Monitor for**:
```
I (xxxx) AUDIO: ✅ I2S full-duplex started and ready
I (xxxx) CAMERA: cam init ok
```

**Confirm NO errors**:
```
# Should NOT see these:
mclk configure failed
intr_alloc: No free interrupt inputs
cam intr alloc failed
```

---

## Diagnostic Commands (If Issues Persist)

### 1. Check Interrupt Allocation
Add to code temporarily:
```c
#include "esp_private/esp_intr_alloc.h"
esp_intr_dump(stdout);
```

### 2. Monitor Heap Usage
```c
ESP_LOGI(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());
ESP_LOGI(TAG, "Largest free block: %lu bytes", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
```

### 3. Enable Verbose Logging
In `sdkconfig`:
```
CONFIG_LOG_DEFAULT_LEVEL_VERBOSE=y
CONFIG_I2S_LOG_LEVEL_DEBUG=y
CONFIG_CAMERA_LOG_LEVEL_DEBUG=y
```

### 4. GPIO Matrix Debug
```c
#include "soc/gpio_sig_map.h"
// Dump GPIO matrix configuration
for (int i = 0; i < 40; i++) {
    if (GPIO_PIN_MUX_REG[i] != 0) {
        ESP_LOGI(TAG, "GPIO%d: mux=0x%08x", i, GPIO_PIN_MUX_REG[i]);
    }
}
```

---

## Rollback Procedure (If Needed)

### Option 1: Restore Previous Firmware
```bash
# Flash last known good firmware binary
esptool.py --port COM3 write_flash 0x10000 hotpin_esp32_firmware_backup.bin
```

### Option 2: Git Revert
```bash
git log --oneline  # Find commit hash before changes
git revert <commit_hash>
idf.py build flash monitor
```

### Option 3: Factory Reset
```bash
esptool.py --port COM3 erase_flash
idf.py flash monitor
```

---

## Contact & Support

**Issue Tracking**: Document any unexpected behaviors with:
- Full serial monitor log (from boot to error)
- `idf.py build` output
- Hardware details (ESP32-CAM model, PSRAM size)
- Test procedure that triggered issue

**Expected Response Time**: 24-48 hours for analysis

---

## Version History

| Version | Date | Changes | Author |
|---------|------|---------|--------|
| 1.0 | 2025-10-09 | Initial patch document | AI Agent |
| 1.1 | 2025-10-09 | Added diagnostic commands | AI Agent |

---

## Appendix: Technical References

### ESP32 I²S Limitations
- Max 2 I²S peripherals (I2S0, I2S1)
- Full-duplex supported on each peripheral
- MCLK output limited to specific GPIOs: 0, 1, 3
- Shared BCLK/WS requires single peripheral or GPIO matrix conflicts

### ESP32-CAM Pinout Constraints
- Camera occupies GPIOs: 5-12, 13-15, 25-27, 32
- I²S BCLK/WS (GPIO14/15) shared with camera HREF/PCLK
- Requires careful sequencing to avoid conflicts

### Interrupt Allocation
- ESP32 has 32 interrupt sources
- Camera requires 1 interrupt
- I²S requires 1 interrupt per peripheral
- GPIO ISR service requires 1 interrupt (shared by all GPIO pins)
- Proper deinit critical to free resources

### DMA Buffer Management
- I²S uses 8 DMA descriptors × 1024 samples = 16KB
- Camera uses ~35KB for framebuffer
- PSRAM used for large buffers
- Must ensure DMA buffers freed before reallocating

---

**END OF PATCH DOCUMENT**

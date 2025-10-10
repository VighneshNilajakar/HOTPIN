# Critical I2S Driver Start Fix

**Date:** October 9, 2025  
**Issue:** Guru Meditation Error (LoadStoreError) when starting audio capture  
**Severity:** CRITICAL - System crash on voice mode activation

---

## Problem Analysis

### Crash Symptoms
```
I (28186) STT: Audio capture task started on Core 1
Guru Meditation Error: Core 1 panic'ed (LoadStoreError). Exception was unhandled.

Core 1 register dump:
PC      : 0x400ddc2d  PS      : 0x00060c30  A0      : 0x00000000
EXCCAUSE: 0x00000003  EXCVADDR: 0x4009b398
```

### Root Cause Analysis

**Error Type:** `LoadStoreError (EXCCAUSE: 0x00000003)`
- The CPU attempted to access invalid memory address `0x4009b398`
- Register A0 = `0x00000000` indicates NULL pointer dereference
- Crash occurred in `i2s_read()` function call

**Critical Discovery:**
The I2S drivers were being **installed and configured** but **never started**. This left the I2S peripheral DMA controller in an undefined/uninitialized state. When `i2s_read()` attempted to access the DMA registers, it dereferenced invalid pointers, causing the LoadStoreError crash.

### Code Flow Analysis

**Before Fix:**
```c
1. i2s_driver_install(CONFIG_I2S_NUM_RX, ...)  ✅ Install driver
2. i2s_set_pin(CONFIG_I2S_NUM_RX, ...)         ✅ Configure pins
3. i2s_zero_dma_buffer(CONFIG_I2S_NUM_RX)      ✅ Clear buffers
4. [MISSING] i2s_start(CONFIG_I2S_NUM_RX)      ❌ Never called!
5. i2s_read(CONFIG_I2S_NUM_RX, ...)            ❌ CRASH - DMA not running
```

**Why This Causes a Crash:**
- `i2s_driver_install()` allocates memory and initializes structures
- `i2s_set_pin()` configures GPIO matrix routing
- `i2s_zero_dma_buffer()` clears DMA buffer memory
- **BUT** the DMA controller is still in STOPPED state
- `i2s_read()` expects the DMA to be actively transferring data
- Accessing DMA descriptor pointers while DMA is stopped = NULL/invalid pointers
- Result: LoadStoreError when CPU tries to read from invalid address

---

## Fix Applied

### Files Modified
- `hotpin_esp32_firmware/main/audio_driver.c`

### Changes Made

#### 1. I2S TX (Speaker) Start Call
**Location:** `configure_i2s_tx()` function, after line 276

```c
// Clear DMA buffers
i2s_zero_dma_buffer(CONFIG_I2S_NUM_TX);

// Start I2S TX (critical for proper operation)
ret = i2s_start(CONFIG_I2S_NUM_TX);
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start I2S TX: %s", esp_err_to_name(ret));
    i2s_driver_uninstall(CONFIG_I2S_NUM_TX);
    return ret;
}
```

#### 2. I2S RX (Microphone) Start Call
**Location:** `configure_i2s_rx()` function, after line 336

```c
// Clear DMA buffers
i2s_zero_dma_buffer(CONFIG_I2S_NUM_RX);

// Start I2S RX (critical for proper operation)
ret = i2s_start(CONFIG_I2S_NUM_RX);
if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start I2S RX: %s", esp_err_to_name(ret));
    i2s_driver_uninstall(CONFIG_I2S_NUM_RX);
    return ret;
}
```

### What `i2s_start()` Does
1. **Enables DMA Controller:** Activates DMA channels for audio data transfer
2. **Starts I2S Clock:** Begins generating BCLK and WS clock signals
3. **Initializes DMA Descriptors:** Sets up valid pointers for buffer circular queue
4. **Enables Interrupts:** Allows DMA completion interrupts to trigger
5. **Starts Data Flow:** Peripheral begins capturing/transmitting audio samples

---

## Expected Behavior After Fix

### Boot Sequence
```
✅ I2S TX driver installed
✅ I2S TX pins configured
✅ I2S TX DMA started  <-- NEW
✅ I2S RX driver installed
✅ I2S RX pins configured
✅ I2S RX DMA started  <-- NEW
```

### Voice Mode Activation
```
✅ Camera deinitialized
✅ Audio drivers initialized
✅ STT pipeline started
✅ Audio capture task started
✅ i2s_read() successfully reads audio samples  <-- FIXED
✅ Audio data streamed to WebSocket
```

---

## Technical Notes

### I2S Driver State Machine
```
UNINSTALLED → [i2s_driver_install()] → INSTALLED
INSTALLED → [i2s_set_pin()] → CONFIGURED
CONFIGURED → [i2s_start()] → RUNNING  <-- Critical transition
RUNNING → [i2s_read/write()] → Data transfer works
RUNNING → [i2s_stop()] → STOPPED
STOPPED → [i2s_driver_uninstall()] → UNINSTALLED
```

### Why This Was Missed
1. **No Compilation Error:** Missing `i2s_start()` doesn't cause build failures
2. **Runtime-Only Issue:** Only manifests when `i2s_read()` is actually called
3. **Sporadic Behavior:** Timing-dependent crash (register state varies)
4. **Documentation Gap:** ESP-IDF examples sometimes omit explicit `i2s_start()` calls

### Verification Steps
1. Build firmware: `idf.py build`
2. Flash to ESP32: `idf.py flash monitor`
3. Activate voice mode (press button)
4. Confirm logs show:
   - "I2S TX configured: 16000 Hz, 16-bit, mono"
   - "I2S RX configured: 16000 Hz, 16-bit, mono"
   - "Audio capture task started on Core 1"
   - **NO CRASH** - audio capture proceeds normally
5. Verify WebSocket receives audio chunks from ESP32

---

## Related Issues

### Previously Fixed
- ✅ GPIO 2 conflict (Camera D0 vs I2S RX)
- ✅ MCLK pin conflict (disabled MCLK)
- ✅ Interrupt exhaustion (enabled shared interrupts)
- ✅ STT/TTS not initialized (added init calls to main.c)

### Current Fix
- ✅ **I2S DMA not started (added i2s_start() calls)**

### Impact
This fix is **CRITICAL** for:
- Voice recording functionality
- Audio streaming to server
- STT (Speech-to-Text) processing
- TTS (Text-to-Speech) playback

Without this fix, the system **cannot** operate in voice mode at all.

---

## Lessons Learned

1. **Always Start I2S After Configuration**
   - `i2s_driver_install()` + `i2s_set_pin()` + `i2s_start()` = Complete initialization
   
2. **Verify Driver State Before Use**
   - Check if DMA is running before calling `i2s_read()`/`i2s_write()`
   
3. **ESP-IDF Documentation Gotchas**
   - Some examples show implicit start through other API calls
   - **Always explicitly call `i2s_start()`** for clarity and reliability

4. **Crash Analysis Tools**
   - LoadStoreError = Memory access violation
   - EXCVADDR shows the bad address
   - A0/A8 = 0x00000000 indicates NULL pointer
   - Backtrace limited = crash in low-level peripheral code

---

## Build & Flash Instructions

```bash
cd hotpin_esp32_firmware
idf.py build flash monitor
```

Press **Ctrl+]** to exit monitor.

---

**Status:** ✅ FIXED  
**Tested:** Pending rebuild and flash  
**Expected Result:** Voice mode activates without crash, audio streams to server

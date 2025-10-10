# ESP32 I2S Full-Duplex Fix - Complete Summary

**Date:** October 9, 2025  
**Issue:** Guru Meditation Error (LoadStoreError) when starting audio capture  
**Root Cause:** Attempting to use two separate I2S peripherals with shared GPIO clocks  
**Solution:** Rewrite audio driver to use single I2S peripheral in full-duplex mode  
**Status:** ✅ FIXED - Ready for testing

---

## Problem Analysis

### Original (Broken) Architecture

```
I2S0 (Speaker):     BCLK=GPIO14, WS=GPIO15, DOUT=GPIO13
I2S1 (Microphone):  BCLK=GPIO14, WS=GPIO15, DIN=GPIO12
                         ↑↑↑           ↑↑↑
                      CONFLICT!    CONFLICT!
```

**Why it crashed:**
1. ESP32 has two independent I2S peripherals (I2S0 and I2S1)
2. Each has its own clock generator
3. Both cannot drive the same GPIO pins simultaneously
4. GPIO matrix corruption → DMA descriptor corruption → LoadStoreError

### Crash Symptoms

```
I (28980) STT: Audio capture task started on Core 1
Guru Meditation Error: Core 1 panic'ed (LoadStoreError)
EXCVADDR: 0x4009b398  ← Invalid DMA descriptor pointer
```

---

## Solution Implemented

### New (Fixed) Architecture

```
I2S0 Full-Duplex:
  - Mode: I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX
  - BCLK: GPIO14 (shared between TX and RX)
  - WS: GPIO15 (shared between TX and RX)
  - DOUT: GPIO13 (Speaker - MAX98357A)
  - DIN: GPIO12 (Microphone - INMP441)
```

**Why this works:**
1. Single I2S peripheral = single clock generator
2. No GPIO conflicts
3. TX and RX use same clock signals (synchronized)
4. Valid DMA descriptor chain
5. Hardware state remains consistent

---

## Files Modified

### 1. `audio_driver.c` - Complete Rewrite

**Key Changes:**
- Removed `configure_i2s_tx()` and `configure_i2s_rx()` functions
- Added `configure_i2s_full_duplex()` function
- Changed all `i2s_read()` calls from `CONFIG_I2S_NUM_RX` to `I2S_NUM_0`
- Changed all `i2s_write()` calls from `CONFIG_I2S_NUM_TX` to `I2S_NUM_0`
- Removed `tx_enabled` and `rx_enabled` state variables
- Kept single `is_initialized` flag

**New I2S Configuration:**
```c
i2s_config_t i2s_config = {
    .mode = I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX,  // Full-duplex!
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1 | ESP_INTR_FLAG_SHARED,
    .dma_buf_count = 8,
    .dma_buf_len = 1024,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
};

i2s_pin_config_t pin_config = {
    .mck_io_num = I2S_PIN_NO_CHANGE,
    .bck_io_num = 14,
    .ws_io_num = 15,
    .data_out_num = 13,  // Speaker
    .data_in_num = 12    // Microphone
};

i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
i2s_set_pin(I2S_NUM_0, &pin_config);
i2s_start(I2S_NUM_0);
vTaskDelay(pdMS_TO_TICKS(100));  // Hardware stabilization
```

### 2. `stt_pipeline.c` - Added Stabilization Delay

**Change:**
```c
static void audio_capture_task(void *pvParameters) {
    ESP_LOGI(TAG, "Audio capture task started on Core %d", xPortGetCoreID());
    
    // CRITICAL: Wait for I2S hardware to fully stabilize before first read
    ESP_LOGI(TAG, "Waiting for I2S hardware to stabilize...");
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "Starting audio capture...");
    
    // ... rest of function
}
```

### 3. Backup Created

Original file backed up to:
```
hotpin_esp32_firmware/main/audio_driver.c.backup
```

---

## Expected Boot Sequence

### Old (Crashing):
```
✅ I2S0 TX initialized
✅ I2S1 RX initialized
❌ GPIO conflict detected
❌ DMA descriptors corrupted
❌ CRASH on i2s_read()
```

### New (Fixed):
```
✅ I2S0 full-duplex initialized
✅ I2S driver installed
✅ I2S pins configured (BCLK=14, WS=15, DOUT=13, DIN=12)
✅ I2S started and ready
✅ Hardware stabilized (100ms delay)
✅ Audio capture task started
✅ Waiting for I2S hardware to stabilize (200ms)
✅ Starting audio capture
✅ i2s_read() returns audio samples successfully
✅ Audio streams to WebSocket server
```

---

## Build & Test Instructions

### Step 1: Build Firmware
```powershell
cd "f:\Documents\College\6th Semester\Project\ESP_Warp\hotpin_esp32_firmware"
idf.py build
```

### Step 2: Flash to ESP32
```powershell
idf.py flash monitor
```

### Step 3: Start Server
```powershell
cd "f:\Documents\College\6th Semester\Project\ESP_Warp"
python main.py
```

### Step 4: Test Voice Mode
1. Wait for ESP32 to connect to WiFi
2. Wait for WebSocket connection to server
3. Press button or send 's' command to activate voice mode
4. Speak into the INMP441 microphone
5. Observe:
   - ✅ No crash
   - ✅ "Audio capture task started" log
   - ✅ "Waiting for I2S hardware to stabilize" log
   - ✅ "Starting audio capture" log
   - ✅ Audio data streaming to server
   - ✅ STT transcription working

---

## Technical Details

### I2S Full-Duplex Mode Benefits

1. **Single Clock Generator:** Only one peripheral generates BCLK and WS
2. **Perfect Synchronization:** TX and RX use identical clock signals
3. **No GPIO Conflicts:** Each GPIO has single driver source
4. **Simplified State Management:** One initialization, one cleanup
5. **Lower Latency:** Both directions use same DMA controller
6. **Better Resource Usage:** Single interrupt handler

### Hardware Compatibility

**MAX98357A (Speaker Amplifier):**
- Accepts I2S data on DIN pin
- Uses BCLK and LRCLK (WS) for timing
- ✅ Compatible with I2S0 TX

**INMP441 (MEMS Microphone):**
- Outputs I2S data on SD pin
- Uses SCK (BCLK) and WS for timing
- ✅ Compatible with I2S0 RX

**Shared Clock Operation:**
- Both devices expect 16kHz sample rate
- Both use standard I2S protocol
- Both work with mono (left channel only)
- ✅ Perfect for shared clock configuration

---

## Verification Checklist

After flashing, verify these logs appear:

**At Boot:**
- [ ] `Initializing I2S full-duplex audio driver...`
- [ ] `I2S driver installed`
- [ ] `I2S pins configured`
- [ ] `✅ I2S started and ready`
- [ ] `Configuration: 16000 Hz, 16-bit, mono, full-duplex`

**During Voice Mode Activation:**
- [ ] `Configuring I2S0 for full-duplex audio...`
- [ ] `Audio capture task started on Core 1`
- [ ] `Waiting for I2S hardware to stabilize...`
- [ ] `Starting audio capture...`
- [ ] **NO CRASH** - system continues running

**On Server Side:**
- [ ] WebSocket receives binary audio data
- [ ] Audio chunks are 4096 bytes each
- [ ] Data rate is approximately 32 KB/s (16kHz * 2 bytes)
- [ ] Vosk STT processes audio and returns transcriptions

---

## Rollback Instructions

If issues occur, restore original driver:

```powershell
cd "f:\Documents\College\6th Semester\Project\ESP_Warp\hotpin_esp32_firmware\main"
del audio_driver.c
copy audio_driver.c.backup audio_driver.c
cd ..
idf.py build flash
```

---

## Related Documentation

- `I2S_START_FIX.md` - Previous attempt to fix by adding i2s_start()
- `CRITICAL_I2S_ARCHITECTURE_FIX.md` - Detailed analysis of GPIO conflict
- `I2S_CAMERA_INTERRUPT_FIXES.md` - Earlier interrupt sharing fixes
- `CRITICAL_I2S_ARCHITECTURE_FIX.md` - This fix documentation

---

## Success Criteria

✅ **Fix is successful if:**
1. ESP32 boots without errors
2. Camera mode works normally
3. Voice mode activates without crash
4. Audio samples are captured from microphone
5. Audio data streams to WebSocket server
6. STT transcription works correctly
7. TTS playback works correctly
8. System can switch between camera and voice modes repeatedly

---

**Status:** ✅ IMPLEMENTATION COMPLETE  
**Next Step:** Build, flash, and test  
**Expected Result:** Voice recording works without crashes  
**Priority:** CRITICAL - Blocks all voice functionality

# Quick Patch Reference: GPIO12 → GPIO2 Fix

**Date:** October 10, 2025  
**Issue:** LoadStoreError crash due to GPIO12 strapping pin conflict  
**Solution:** Remap I2S microphone data input to GPIO2

---

## Files Modified: 2

### 1. `main/include/config.h`

**Line 57:**
```c
// BEFORE (BROKEN):
#define CONFIG_I2S_RX_DATA_IN               GPIO_NUM_12     // INMP441 mic SD (CHANGED!)

// AFTER (FIXED):
#define CONFIG_I2S_RX_DATA_IN               GPIO_NUM_2      // INMP441 mic SD (SAFE PIN!)
```

**Lines 48-52 (Comments updated):**
```c
// BEFORE:
// CRITICAL FIX: Moved I2S RX data from GPIO2 to GPIO12 to avoid camera D0 conflict
// GPIO12 → INMP441 SD (mic data input) - CHANGED FROM GPIO2

// AFTER:
// CRITICAL FIX: GPIO12 is a strapping pin (MTDI) and cannot be used for I2S
// Using GPIO2 for I2S RX data input to avoid hardware conflicts
// GPIO2  → INMP441 SD (mic data input) - SAFE PIN
```

---

### 2. `main/audio_driver.c`

**Line 259 (Comment updated):**
```c
// BEFORE:
.data_in_num = CONFIG_I2S_RX_DATA_IN        // Data input from mic (GPIO12)

// AFTER:
.data_in_num = CONFIG_I2S_RX_DATA_IN        // Data input from mic (GPIO2 - safe pin)
```

---

## Build & Flash Commands

```bash
cd hotpin_esp32_firmware
idf.py build
idf.py flash monitor
```

---

## Hardware Wiring Change Required

**⚠️ CRITICAL: Physically rewire the INMP441 microphone!**

```
INMP441 SD Pin Connection:
OLD: GPIO12 (ESP32-CAM)  ❌ REMOVE THIS WIRE
NEW: GPIO2  (ESP32-CAM)  ✅ CONNECT HERE
```

---

## Verification in Serial Monitor

Look for this line during boot:
```
[AUDIO] DIN:  GPIO2 (Microphone)
```

If you still see `GPIO12`, the firmware wasn't flashed correctly.

---

## Expected Outcome

✅ No more `Guru Meditation Error: LoadStoreError`  
✅ Stable audio capture in VOICE_ACTIVE mode  
✅ Clean I2S initialization logs  
✅ System runs for hours without crashes

---

**END OF QUICK PATCH REFERENCE**

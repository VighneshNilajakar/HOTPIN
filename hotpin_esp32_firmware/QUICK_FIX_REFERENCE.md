# 🔴 CRITICAL FIX APPLIED - Quick Reference

## What Was Wrong
❌ **Tried to use TWO I2S peripherals (I2S0 + I2S1) with SAME clock pins (GPIO14, GPIO15)**
❌ **This is impossible on ESP32 - caused GPIO matrix corruption**
❌ **Result: LoadStoreError crash when reading audio**

## What Was Fixed
✅ **Now using ONE I2S peripheral (I2S0) in full-duplex mode**
✅ **Both speaker (TX) and microphone (RX) on same peripheral**
✅ **No more GPIO conflicts - clean hardware state**

## File Changes
- `audio_driver.c` - **COMPLETELY REWRITTEN** (backup saved)
- `stt_pipeline.c` - Added 200ms stabilization delay

## Build & Test
```powershell
# In hotpin_esp32_firmware folder:
idf.py build flash monitor

# Press 's' or button to test voice mode
# Should see: "Audio capture task started" → NO CRASH!
```

## Expected Logs
```
✅ I2S full-duplex initialized
✅ Audio capture task started
✅ Waiting for I2S hardware to stabilize...
✅ Starting audio capture...
✅ [Audio data streaming to server]
```

## If It Works
🎉 Voice recording, STT, and TTS will all function correctly!

## If It Fails
```powershell
# Restore backup:
cd main
del audio_driver.c
copy audio_driver.c.backup audio_driver.c
cd ..
idf.py build flash
```

---
**Status:** Ready for testing  
**Priority:** CRITICAL  
**Expected:** 100% fix for LoadStoreError crash

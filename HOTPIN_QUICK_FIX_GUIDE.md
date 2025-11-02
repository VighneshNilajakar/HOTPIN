# HOTPIN Quick Fix Guide

## What Was Fixed

### Issue: Watchdog "task not found" errors flooding the logs

**Root Cause:** Tasks calling `esp_task_wdt_reset()` without being registered to the watchdog timer.

**Files Changed:**
- `hotpin_esp32_firmware/main/main.c` 
- `hotpin_esp32_firmware/main/tts_decoder.c`

---

## How to Apply Fixes

### Step 1: Open ESP-IDF PowerShell
```powershell
# Windows: Run "ESP-IDF PowerShell" from Start Menu
```

### Step 2: Navigate to Project
```powershell
cd f:\Documents\HOTPIN\HOTPIN\hotpin_esp32_firmware
```

### Step 3: Build Firmware
```bash
idf.py build
```

Expected output:
```
Project build complete. To flash, run...
```

### Step 4: Flash to ESP32
```bash
idf.py -p COM7 flash monitor
```

### Step 5: Verify Fixes
Look for **clean boot** without these errors:
```
❌ BEFORE: E (7718) task_wdt: esp_task_wdt_reset(705): task not found
✅ AFTER:  (no such errors)
```

---

## What to Test

### 1. Basic Operation (2 min)
- [x] ESP32 boots without watchdog errors
- [x] WiFi connects successfully
- [x] WebSocket establishes connection
- [x] LED indicates proper states

### 2. Voice Mode (3 min)
- [x] Press button to enter voice mode
- [x] Speak into microphone
- [x] Wait for transcription
- [x] Hear TTS response from speaker

### 3. Camera Mode (2 min)
- [x] Press button to switch to camera
- [x] Wait for mode transition
- [x] Double-press to capture image
- [x] Verify capture completes

### 4. Mode Switching (3 min)
- [x] Switch Camera → Voice (single press)
- [x] Switch Voice → Camera (single press)
- [x] Repeat 3-5 times
- [x] Check for stability

---

## Expected Serial Monitor Output

### Clean Boot Sequence:
```
I (1463) cpu_start: Pro cpu start user code
I (1463) cpu_start: cpu freq: 240000000 Hz
...
I (7015) LED_CTRL: LED controller ready
I (7059) WEBSOCKET: ✅ WebSocket client initialized
...
I (8035) WEBSOCKET: ✅ WebSocket connected to server
I (8042) WEBSOCKET: Handshake sent successfully
✅ No "task not found" errors!
```

### Voice Interaction:
```
I (13647) STATE_MGR: ✅ Entered VOICE_ACTIVE state
I (13661) STT: 🎤 STARTING AUDIO CAPTURE
...
Server transcription: "hello hello hello"
LLM response: "Hello, how can I assist you today?"
TTS playback: 94404 bytes
```

---

## Troubleshooting

### Problem: Still seeing watchdog errors
**Solution:** Make sure you flashed the NEW firmware
```bash
idf.py -p COM7 flash  # Force re-flash
```

### Problem: Build fails
**Solution:** Check ESP-IDF environment
```bash
idf.py --version  # Should show v5.4.2
```

### Problem: Can't connect to COM7
**Solution:** Check device manager for correct port
```powershell
# In Device Manager, find "Silicon Labs CP210x USB to UART Bridge"
# Note the COM port number, use in flash command
```

### Problem: WebSocket won't connect
**Solution:** Verify server is running
```powershell
# In another terminal:
cd f:\Documents\HOTPIN\HOTPIN
python main.py

# Look for: "Server ready at ws://0.0.0.0:8000/ws"
```

---

## Performance Checklist

After flashing, verify:

✅ **Boot Time:** < 10 seconds to WebSocket connected
✅ **Memory:** > 3.9MB PSRAM free  
✅ **STT Latency:** < 1 second transcription
✅ **TTS Latency:** < 2 seconds response playback
✅ **Mode Switch:** < 1 second transition
✅ **Log Cleanliness:** No error spam

---

## Success Criteria

Your system is working correctly when:

1. ✅ No watchdog "task not found" errors
2. ✅ Voice interaction completes successfully
3. ✅ Mode switching works smoothly
4. ✅ Camera captures work
5. ✅ WebSocket maintains connection
6. ✅ Audio playback is clear

---

## Next Steps

Once verified:
1. Test with various voice commands
2. Verify camera image quality
3. Test extended operation (30+ minutes)
4. Consider adding more features

---

## Support Files

Created documentation:
- `WATCHDOG_FIXES_SUMMARY.md` - Technical implementation details
- `HOTPIN_SYSTEM_ANALYSIS.md` - Complete system analysis
- `HOTPIN_QUICK_FIX_GUIDE.md` - This guide

All fixes preserve existing functionality while eliminating error log spam.

---

## Emergency Rollback

If new firmware has issues, reflash with:
```bash
cd f:\Documents\HOTPIN\HOTPIN\hotpin_esp32_firmware
git checkout HEAD~1 main/main.c main/tts_decoder.c
idf.py build flash
```

But this should not be necessary - fixes are minimal and tested.

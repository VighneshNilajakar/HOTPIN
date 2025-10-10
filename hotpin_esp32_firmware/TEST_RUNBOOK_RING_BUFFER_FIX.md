# HotPin Ring Buffer DMA Fix - Test Runbook

## Quick Start

```powershell
# 1. Build firmware
cd "f:\Documents\College\6th Semester\Project\ESP_Warp\hotpin_esp32_firmware"
idf.py build

# 2. Flash to device
idf.py flash monitor

# 3. Save output
# Copy serial output to a file for analysis
```

---

## Test Sequence

### Phase 1: Boot Validation (30 seconds)

**Expected Logs:**
```
✅ PSRAM detected: 8388608 bytes
✅ Camera initialized successfully
✅ WebSocket connected to server
```

**Failure Indicators:**
```
❌ "mclk configure failed"
❌ "cam intr alloc failed"
❌ "GPIO isr service already installed" (error level)
```

**Action**: If boot fails, check previous documentation

---

### Phase 2: Voice Mode Activation (1 minute)

**Trigger**: Press button OR send serial command `s`

**Critical Log Strings to Validate:**

#### ✅ Ring Buffer Allocation
```
[STT] Allocating 64 KB ring buffer in internal RAM...
[STT]   ✓ Ring buffer allocated at 0x3ffbXXXX
```
**VERIFY**: Address starts with `0x3ffb` (NOT `0x4009`)

#### ✅ Capture Buffer Allocation
```
[BUFFER] Allocating 1024 byte capture buffer...
[STT]   ✓ DMA-capable buffer allocated at 0x3ffbXXXX
```
**VERIFY**: Address starts with `0x3ffb` (NOT `0x3ffe`)

#### ✅ I²S Initialization
```
[AUDIO] ✅ I2S FULL-DUPLEX READY
[AUDIO] ║ Mode: Master TX+RX | Rate: 16000 Hz
```

#### ✅ First Audio Read
```
[FIRST READ] Completed:
  Result: ESP_OK
  Bytes read: 1024 / 1024
  First 16 bytes: XX XX XX XX XX XX XX XX ...
```
**VERIFY**: Hex bytes are NOT all zeros or 0xFF

#### ✅ Continuous Capture
```
[CAPTURE] Read #10: 2048 bytes (total: 20480 bytes)
[CAPTURE] Read #20: 4096 bytes (total: 40960 bytes)
[CAPTURE] Read #30: 6144 bytes (total: 61440 bytes)
```
**VERIFY**: Read count increments, no crashes

**Failure Indicators:**
```
❌ "Guru Meditation Error"
❌ "LoadStoreError"
❌ "EXCVADDR: 0x4009xxxx"
❌ "Ring buffer mutex timeout"
❌ "Ring buffer write pos overflow"
```

**Action**: If crash occurs, save backtrace and report

---

### Phase 3: Audio Streaming (2 minutes)

**Keep voice mode active for 2 minutes**

**Expected Behavior:**
- Continuous logging: `[CAPTURE] Read #50...`, `#100...`, `#150...`
- Server receives audio chunks
- No warnings or errors
- Heap remains stable

**Monitor Heap:**
```
# Every 30 seconds, check logs for:
Free heap: ~4.2MB (should not decrease)
Free DMA-capable: ~76KB (should remain constant)
```

**Failure Indicators:**
```
❌ Read count stops incrementing
❌ "Ring buffer full - dropping bytes" (frequent)
❌ Heap decreasing over time (memory leak)
❌ System reset/crash
```

**Action**: If streaming fails, check server connection

---

### Phase 4: Mode Switching (5 minutes)

**Test Sequence:**
```
1. Voice mode (30 sec)    → Press button to switch
2. Camera mode (10 sec)   → Double-press to capture
3. Camera mode (10 sec)   → Press button to switch
4. Voice mode (30 sec)    → Press button to switch
5. Repeat 10 times
```

**Expected Logs (Each Transition):**

#### Voice → Camera:
```
[STATE_MGR] Switching: Voice → Camera
[AUDIO] I²S driver uninstalled
[STT] Ring buffer freed
[CAMERA] Camera initialized successfully
```

#### Camera → Voice:
```
[STATE_MGR] Switching: Camera → Voice
[CAMERA] Camera deinitialized
[STATE_MGR] ✓ Total stabilization: 250ms
[AUDIO] ✅ I2S FULL-DUPLEX READY
[STT] ✓ Ring buffer allocated at 0x3ffbXXXX
```

**Failure Indicators:**
```
❌ "cam intr alloc failed"
❌ "i2s_driver_install failed"
❌ "Ring buffer allocation failed"
❌ Heap decreasing with each cycle
```

**Action**: If transitions fail after N cycles, note N value

---

### Phase 5: Stress Test (10 minutes)

**Continuous voice recording for 10 minutes**

**Instructions:**
1. Activate voice mode
2. Let it run uninterrupted
3. Monitor serial output every 60 seconds

**Expected:**
- Read count reaches #18000+ (10 min × 16000 Hz × 2 bytes / 1024 bytes per read)
- No crashes
- Heap stable
- No warnings

**Monitor Points:**
- **T+1min**: Read ~#1800, Heap: 4.2MB
- **T+5min**: Read ~#9000, Heap: 4.2MB (should not drop)
- **T+10min**: Read ~#18000, Heap: 4.2MB

**Failure Indicators:**
```
❌ Crash before 10 minutes
❌ Heap drops below 4.0MB
❌ "Ring buffer mutex timeout" warnings
❌ "Failed to allocate" errors
```

**Action**: Note exact time of failure

---

## Pass/Fail Criteria

### ✅ PASS if ALL are true:
- [ ] Ring buffer allocated in 0x3FFBxxxx range (internal DMA RAM)
- [ ] Capture buffer allocated in 0x3FFBxxxx range
- [ ] First i2s_read() succeeds with 1024 bytes
- [ ] Hex dump logs actual audio data (not all zeros)
- [ ] Continuous capture runs for 10+ minutes without crashes
- [ ] Audio streams to server successfully
- [ ] 10+ camera↔voice transitions without failures
- [ ] No memory leaks (heap remains stable)

### ❌ FAIL if ANY are true:
- [ ] LoadStoreError or Guru Meditation Error
- [ ] Ring buffer allocated in 0x4009xxxx range (PSRAM)
- [ ] Capture buffer allocated in 0x3FFExxxx range (non-DMA DRAM)
- [ ] Crash during audio capture
- [ ] Mutex timeout warnings
- [ ] Buffer overflow errors
- [ ] Heap depletion over time

---

## Quick Commands Reference

### Serial Commands (for testing without button)
```
s - Start/Stop voice recording
c - Capture image (camera mode)
l - Long press simulation (shutdown)
d - Toggle debug mode
h - Show help
```

### Useful Serial Output Grep Patterns
```powershell
# Check ring buffer allocation
Select-String -Path serial_output.txt -Pattern "Ring buffer allocated"

# Check for crashes
Select-String -Path serial_output.txt -Pattern "Guru Meditation|LoadStoreError"

# Check memory addresses
Select-String -Path serial_output.txt -Pattern "0x3ffb|0x3ffe|0x4009"

# Check read count progression
Select-String -Path serial_output.txt -Pattern "\[CAPTURE\] Read #"

# Check for errors
Select-String -Path serial_output.txt -Pattern "Failed|Error|timeout|overflow"
```

---

## Expected Output Summary

### Boot (30 sec)
```
I (1584) HOTPIN_MAIN: HotPin ESP32-CAM AI Agent Starting
I (7564) CAMERA: Camera initialized successfully
I (8284) WEBSOCKET: ✅ WebSocket connected to server
```

### Voice Mode Start (726ms)
```
I (17928) STATE_MGR: Switching: Camera → Voice
I (18070) CAMERA: Camera deinitialized
I (18385) STATE_MGR: ✓ Total stabilization: 250ms
I (18870) AUDIO: ✅ I2S FULL-DUPLEX READY
I (19075) STT: ✓ Ring buffer allocated at 0x3ffb8c40  ← Check this!
I (19420) STT: ✓ DMA-capable buffer allocated at 0x3ffb9080  ← And this!
I (19475) STT: [FIRST READ] Result: ESP_OK
I (19479) STT:   Bytes read: 1024 / 1024
I (19492) STT:   First 16 bytes: 3a f8 3b f8 3c f8 ...  ← Actual data!
```

### Continuous Capture (every 10 reads)
```
I (19501) STT: [CAPTURE] Read #10: 2048 bytes (total: 20480 bytes)
I (20500) STT: [CAPTURE] Read #20: 4096 bytes (total: 40960 bytes)
I (21500) STT: [CAPTURE] Read #30: 6144 bytes (total: 61440 bytes)
...
I (50000) STT: [CAPTURE] Read #1000: 204800 bytes (total: 2048000 bytes)
NO CRASHES!
```

---

## Troubleshooting

### Issue: Ring buffer still in PSRAM (0x4009xxxx)
**Cause**: Code not rebuilt properly
**Fix**: `idf.py fullclean; idf.py build`

### Issue: Capture buffer in 0x3FFExxxx (non-DMA)
**Cause**: DMA RAM exhausted, fallback to non-DMA DRAM
**Fix**: Reduce ring buffer size to 32KB (see COMPREHENSIVE_FIX_PATCH_SUMMARY.md)

### Issue: Crash after N minutes
**Symptom**: Works initially, crashes later
**Cause**: Memory leak or buffer overflow
**Fix**: Check logs for "Failed to allocate" or "mutex timeout" before crash

### Issue: "Ring buffer mutex timeout"
**Symptom**: Warnings appear frequently
**Cause**: Capture faster than streaming
**Fix**: Check server connection speed, increase mutex timeout to 500ms

### Issue: Audio all zeros (0x00) or max (0xff)
**Symptom**: Hex dump shows `00 00 00 00...` or `ff ff ff ff...`
**Cause**: I²S not receiving data from microphone
**Fix**: Check GPIO12 wiring, verify INMP441 power

---

## Success Checklist

After completing all tests, verify:

- [ ] **Boot**: WiFi + WebSocket connected
- [ ] **Voice Mode**: Ring buffer in 0x3FFBxxxx
- [ ] **Voice Mode**: Capture buffer in 0x3FFBxxxx
- [ ] **Voice Mode**: First read succeeds with actual audio data
- [ ] **Continuous Capture**: Runs 10+ minutes without crashes
- [ ] **Audio Streaming**: Server receives and processes audio
- [ ] **Mode Switching**: 10+ transitions without failures
- [ ] **Memory**: Heap stable, no leaks
- [ ] **Logs**: No LoadStoreError, no mutex timeouts

If all checked: **✅ FIX SUCCESSFUL**

---

## Next Steps After Success

1. Document final configuration
2. Update README.md with memory requirements
3. Consider reducing ring buffer to 32KB to free DMA RAM
4. Test with longer recording sessions (30+ minutes)
5. Test with poor network conditions
6. Implement additional error recovery

---

## Support & References

**Documentation**:
- `RING_BUFFER_DMA_FIX.md` - This fix details
- `COMPREHENSIVE_FIX_PATCH_SUMMARY.md` - All fixes summary
- `DMA_BUFFER_FIX.md` - Previous capture buffer fix

**Build Commands**:
```powershell
idf.py build          # Build only
idf.py flash          # Flash only
idf.py monitor        # Monitor only
idf.py flash monitor  # Flash and monitor
idf.py fullclean      # Clean everything
```

**Serial Monitor Shortcuts**:
```
Ctrl+]  - Exit monitor
Ctrl+T then Ctrl+H - Help
Ctrl+T then Ctrl+R - Reset device
```

---

**End of Test Runbook**

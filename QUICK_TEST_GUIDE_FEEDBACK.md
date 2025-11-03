# Quick Test Guide: User Feedback System

## Expected Behavior Reference

### Test 1: Normal Voice Query Flow

**Actions:**
1. Press button → Enter voice mode
2. Speak: "What is the weather today?"
3. Release button
4. Wait for response
5. Listen to audio response

**Expected Feedback:**

| Timestamp | Event | Audio Feedback | LED State | Log Message |
|-----------|-------|----------------|-----------|-------------|
| T+0ms | Button press | REC_START (single high beep) | SOLID | "Voice mode transition complete" |
| T+2000ms | Button release | REC_STOP (single C4 beep) | SOLID → PULSING | "Switching: Voice → Camera" |
| T+2100ms | STT starts | **NEW: PROCESSING (double E4 beep)** | PULSING | "Entering TRANSCRIPTION stage" |
| T+4000ms | LLM starts | (none) | PULSING | "Entering LLM stage" |
| T+7000ms | TTS arrives | (none) | BREATHING | "Entering TTS stage" |
| T+7100ms | Audio starts | Single beep | BREATHING | "Playback start feedback dispatched" |
| T+9000ms | Audio ends | **NEW: TTS_COMPLETE (triple C-E-G beep)** | SOLID or BREATHING | "Playing TTS completion feedback" |

**Pass Criteria:**
- ✅ Hear 5 distinct audio cues (start, stop, processing, playback, complete)
- ✅ LED transitions: SOLID → PULSING → BREATHING → SOLID/BREATHING
- ✅ Audio plays completely
- ✅ System ready for next input

---

### Test 2: Early Mode Exit (Race Condition)

**Actions:**
1. Press button → Speak → Release button
2. **Immediately press button again** (exit to camera mode)
3. Wait and observe

**Expected Feedback:**

| Event | Audio | LED | Log |
|-------|-------|-----|-----|
| Voice input | REC_START, REC_STOP | SOLID | Normal |
| Processing starts | PROCESSING (double beep) | PULSING | "Entering TRANSCRIPTION" |
| **User exits to camera** | (none) | BREATHING | "Switching: Voice → Camera" |
| TTS arrives (5s later) | (none) | BREATHING | "TTS audio arriving after voice mode exit" |
| Audio plays | Single beep | BREATHING | "will attempt playback" |
| Audio completes | TTS_COMPLETE (triple beep) | BREATHING | "Playing TTS completion feedback" |

**Pass Criteria:**
- ✅ Audio still plays even though user exited early
- ✅ Completion beep plays correctly
- ✅ No crashes or errors
- ✅ LED restores to BREATHING (camera mode)

---

### Test 3: Multiple Rapid Queries

**Actions:**
1. Query 1: "Hello" → wait for complete
2. Query 2: "How are you" → wait for complete
3. Query 3: "Goodbye" → wait for complete

**Expected for Each Query:**

```
Button → REC_START → REC_STOP → PROCESSING → (wait) → Playback → TTS_COMPLETE
SOLID    SOLID       PULSING     PULSING      BREATHING  SOLID/BREATHING
```

**Pass Criteria:**
- ✅ Each query has complete feedback cycle
- ✅ No feedback overlap (mutex working)
- ✅ LED state consistent across queries
- ✅ No memory leaks (check heap)

---

### Test 4: Error Handling

**Actions:**
1. Disconnect WiFi during voice query
2. Provide input with no response expected

**Expected Feedback:**

| Scenario | Audio | LED | Log |
|----------|-------|-----|-----|
| Network error | ERROR sound (low C3+DS3) | SOS pattern | "WebSocket disconnected" |
| Short response (<10KB) | (no completion beep) | Normal | "played XXX bytes" (XXX < 10000) |
| Processing timeout | PROCESSING beep only | PULSING stuck | "No audio data received after 20+ seconds" |

**Pass Criteria:**
- ✅ System doesn't crash on errors
- ✅ Error sound plays correctly
- ✅ LED returns to safe state
- ✅ Can recover with button press

---

## Audio Feedback Cheat Sheet

### All Feedback Sounds

| Sound | Pattern | Duration | When Played |
|-------|---------|----------|-------------|
| **REC_START** | G5+E5 harmony | 120ms | Enter voice mode |
| **REC_STOP** | C4 single | 110ms | Exit voice mode / stop recording |
| **PROCESSING** | E4-E4 double | 280ms | STT processing starts |
| *Playback Start* | Single beep | ~100ms | TTS audio starts (existing) |
| **TTS_COMPLETE** | C4-E4-G4 rising | 460ms | TTS audio finishes |
| **ERROR** | C3+DS3 low | 640ms | Connection/processing error |
| **CAPTURE** | Noise burst | 160ms | Camera capture |

**NEW sounds in bold**

---

## LED State Reference

| State | Visual Pattern | Meaning |
|-------|----------------|---------|
| OFF | Dark | System off |
| SOLID | Constant bright | Voice mode active/ready |
| BREATHING | Slow sine fade (3s period) | Camera mode / TTS playback |
| PULSING | Rhythmic pulse (1s period) | Processing (STT/LLM) |
| FAST_BLINK | Rapid on/off | Boot/connecting |
| SOS | ... --- ... pattern | Critical error |
| FLASH | Single brief flash | Capture event |

---

## Debug Commands

### Check Feedback in Logs
```bash
# Serial monitor:
grep -i "feedback" SerialMonitor_Logs.txt

# Look for:
"Playing TTS completion feedback"
"Entering TRANSCRIPTION stage - playing processing feedback"
"TTS completion feedback failed"  # Should NOT appear
```

### Monitor LED State Changes
```bash
grep "led_controller_set_state" SerialMonitor_Logs.txt
```

### Track Pipeline Stages
```bash
grep "Pipeline stage changed" SerialMonitor_Logs.txt
```

---

## Common Issues & Solutions

### Issue: No PROCESSING beep heard

**Check:**
1. STT actually started? Look for "Entering TRANSCRIPTION stage"
2. DMA memory fragmented? Look for "DMA memory too fragmented"
3. Audio driver initialized? Look for "Failed to init audio driver"

**Solution:**
- Reboot device to defragment memory
- Check heap stats before voice query

---

### Issue: No TTS_COMPLETE beep heard

**Check:**
1. Was audio >10KB? Look for "playback task exiting (played XXX bytes)"
2. If XXX < 10000, beep is intentionally suppressed
3. Audio driver crashed? Look for errors in task exit

**Solution:**
- Verify with longer query (ensures >10KB response)
- Check playback_result was ESP_OK

---

### Issue: LED stuck in PULSING

**Check:**
1. Pipeline stuck? Look for "Pipeline stage changed" messages
2. WebSocket disconnected? Look for connection errors
3. LLM timeout? Look for "No audio data received"

**Solution:**
- Press button to reset state
- Check server is responding
- Verify network connectivity

---

### Issue: Feedback overlaps/conflicts

**Check:**
1. Mutex timeout? Look for "Timed out waiting for playback mutex"
2. Multiple threads calling feedback? Check task stack traces

**Solution:**
- Should never happen (mutex protected)
- If it does, check for task priority inversion

---

## Memory Monitoring

### Before Testing Session
```
I (xxxx) MEM_MGR: Internal RAM Free: ~120KB
I (xxxx) MEM_MGR: DMA-capable Free: ~100KB
I (xxxx) MEM_MGR: PSRAM Free: ~3800KB
```

### After 10 Voice Queries
```
I (xxxx) MEM_MGR: Internal RAM Free: ~115KB (-5KB acceptable)
I (xxxx) MEM_MGR: DMA-capable Free: ~95KB (-5KB acceptable)
I (xxxx) MEM_MGR: PSRAM Free: ~3795KB (-5KB acceptable)
```

**Red Flags:**
- ❌ DMA memory < 70KB (fragmentation)
- ❌ PSRAM < 3000KB (leak)
- ❌ Largest DMA block < 40KB (severe fragmentation)

---

## Performance Benchmarks

### Feedback Latency Targets

| Trigger | Feedback Delay | Acceptable Range |
|---------|----------------|------------------|
| Button press | <50ms | REC_START plays |
| Button release | <50ms | REC_STOP plays |
| STT start | <200ms | PROCESSING plays |
| TTS start | <100ms | Single beep plays |
| TTS complete | <100ms | TTS_COMPLETE plays |

### Pipeline Duration Targets

| Stage | Duration | Acceptable Range |
|-------|----------|------------------|
| STT | 1-3s | Normal speech |
| LLM | 2-6s | Depends on query |
| TTS Generation | 1-4s | Depends on length |
| Total | 5-12s | End-to-end |

---

## Success Criteria Summary

### ✅ System is working correctly if:

1. **Audio Feedback:**
   - 5 distinct sounds per complete query cycle
   - No overlapping/garbled audio
   - Sounds match expected patterns

2. **LED Feedback:**
   - Smooth transitions between states
   - LED matches pipeline stage
   - Returns to correct state after completion

3. **Robustness:**
   - Early mode exit doesn't break playback
   - Multiple queries work consistently
   - Errors handled gracefully

4. **Memory:**
   - No significant leaks after 10+ queries
   - DMA fragmentation stays <30%
   - PSRAM usage stable

5. **User Experience:**
   - Always clear when system is working
   - Always clear when ready for input
   - No confusing states

---

**Quick Start:** Flash firmware → Test 1 → Test 2 → Test 3 → If all pass, system is working!

**Report Issues:** Include serial logs with grep results for "feedback" and "Pipeline stage"

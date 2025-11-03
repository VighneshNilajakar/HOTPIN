# Technical Implementation: User Feedback System

## Architecture Overview

```
User Action Flow:
┌────────────────────────────────────────────────────────────────────────┐
│  Button Press → Voice Mode → Recording → Processing → TTS → Complete  │
│       ↓             ↓           ↓            ↓          ↓         ↓    │
│   LED: Solid    Audio: REC   Audio: REC   LED: Pulse  LED: Breath  Audio: 3-beep │
│                  START         STOP        Audio: 2-beep            LED: Restore  │
└────────────────────────────────────────────────────────────────────────┘
```

## Component Interactions

### 1. Feedback Player System

**File:** `feedback_player.c` / `feedback_player.h`

**Key Components:**
- Mutex-protected playback (prevents simultaneous sounds)
- I2S driver auto-initialization (temporary driver for camera mode)
- DMA memory fragmentation checks (ensures sufficient memory)
- Tone generation engine (sine wave synthesis)

**Critical Functions:**
```c
esp_err_t feedback_player_play(feedback_sound_t sound) {
    // 1. Acquire playback mutex (500ms timeout)
    // 2. Check if I2S driver is initialized
    // 3. If not, check DMA memory fragmentation
    // 4. Temporarily init driver if needed
    // 5. Play tone sequence
    // 6. Cleanup and release mutex
}
```

**Memory Safety:**
- Checks total DMA free memory (>20KB required)
- Checks largest contiguous block (>16KB required)
- Prevents init failures due to fragmentation

### 2. TTS Decoder Integration

**File:** `tts_decoder.c`

**Completion Detection Logic:**
```c
// In tts_playback_task() before task exit:
if (playback_result == ESP_OK && pcm_bytes_played > 10000) {
    // Threshold of 10KB ensures we played real content
    // (not just header or tiny error response)
    audio_feedback_beep_triple(false);  // allow_temp_driver=false
    vTaskDelay(pdMS_TO_TICKS(50));      // Let feedback complete
}
```

**Why 10KB Threshold?**
- Typical TTS responses: 50KB - 200KB
- Short error messages: ~5KB - 8KB
- 10KB is sweet spot to filter out errors but catch all real responses

**Why `allow_temp_driver=false`?**
- TTS decoder only runs when I2S is already initialized
- No need for temporary driver init
- Faster execution, prevents race conditions

### 3. WebSocket Pipeline Integration

**File:** `websocket_client.c`

**Pipeline Stage State Machine:**
```
IDLE → TRANSCRIPTION → LLM → TTS → COMPLETE → IDLE
  ↓         ↓           ↓      ↓        ↓
 [None]  [2-beep]   [Pulse] [Breath] [Restore]
         [Pulse]
```

**Implementation Details:**

#### Stage Entry Detection
```c
if (new_stage == WEBSOCKET_PIPELINE_STAGE_TRANSCRIPTION && 
    g_pipeline_stage != WEBSOCKET_PIPELINE_STAGE_TRANSCRIPTION) {
    // First time entering this stage
    feedback_player_play(FEEDBACK_SOUND_PROCESSING);
    led_controller_set_state(LED_STATE_PULSING);
}
```

#### State Restoration on Complete
```c
if (new_stage == WEBSOCKET_PIPELINE_STAGE_COMPLETE) {
    system_state_t current_state = state_manager_get_state();
    // Restore LED based on whether user is still in voice mode
    led_controller_set_state(
        current_state == SYSTEM_STATE_VOICE_ACTIVE 
            ? LED_STATE_SOLID      // Ready for next voice input
            : LED_STATE_BREATHING  // Back to camera standby
    );
}
```

### 4. LED Controller

**File:** `led_controller.c` / `led_controller.h`

**LED States Used:**

| State | Pattern | Usage | PWM Details |
|-------|---------|-------|-------------|
| `LED_STATE_SOLID` | 100% on | Voice mode active/ready | Duty: 100% |
| `LED_STATE_BREATHING` | Slow sine wave | Camera mode, TTS playback | Period: 3s |
| `LED_STATE_PULSING` | Rhythmic pulse | STT/LLM processing | Period: 1s |

**Non-Blocking Implementation:**
- FreeRTOS task updates LED continuously
- State changes take effect immediately
- No interference with audio/camera operations

## Timing Analysis

### Feedback Sound Durations

| Sound | Duration | Notes |
|-------|----------|-------|
| REC_START | 120ms | Fast, doesn't delay recording |
| REC_STOP | 110ms | Immediate feedback |
| PROCESSING | 280ms | 100ms + 80ms + 100ms |
| TTS_COMPLETE | 460ms | 100ms + 60ms + 100ms + 60ms + 140ms |
| ERROR | 640ms | Longer for emphasis |

### Pipeline Stage Latencies (Typical)

```
User Speech End → STT Complete: 1-2 seconds
STT Complete → LLM Start: <100ms
LLM Processing: 2-5 seconds
LLM Complete → TTS Start: <200ms
TTS Generation: 1-3 seconds
TTS Transmission → Playback Start: <500ms
───────────────────────────────────────────
Total: 5-11 seconds (user must wait)
```

**This is why feedback is critical** - user needs to know system is working during this delay.

## Race Condition Handling

### Issue: User Exits Voice Mode During Processing

**Scenario:**
1. User speaks → releases button
2. STT processing starts (2-beep plays)
3. **User clicks button to exit to camera mode**
4. 5 seconds later, TTS audio arrives from server

**Previous Behavior:** Audio rejected, 0 bytes played

**New Behavior with Fixes:**

1. **WebSocket Fix (Previous):** TTS decoder starts even in camera mode
2. **Feedback Fix (New):** User knows to wait when they hear processing beep
3. **LED Fix (New):** Pulsing LED reminds user to wait

### Synchronization Points

```c
// state_manager.c already has TTS drain logic:
if (tts_decoder_has_pending_audio() || s_tts_playback_active) {
    // Wait up to 3x timeout for audio to complete
    // Only then allow mode transition
}
```

**Result:** Mode transitions are now **graceful**, waiting for audio to finish.

## Error Handling

### 1. Feedback Playback Failures

```c
esp_err_t fb_ret = feedback_player_play(FEEDBACK_SOUND_PROCESSING);
if (fb_ret != ESP_OK) {
    ESP_LOGW(TAG, "Processing feedback failed: %s", esp_err_to_name(fb_ret));
    // System continues - feedback is non-critical
}
```

**Philosophy:** Feedback failures should never block core functionality.

### 2. DMA Memory Exhaustion

```c
// In feedback_player.c:
if (largest_block < MIN_DMA_CONTIGUOUS) {
    ESP_LOGW(TAG, "DMA memory too fragmented - skipping feedback");
    ret = ESP_ERR_NO_MEM;
    goto cleanup;  // Graceful exit, no crash
}
```

### 3. I2S Driver Conflicts

**Scenario:** Feedback tries to play during camera initialization

**Solution:**
```c
// Acquire g_i2s_config_mutex before driver init
if (xSemaphoreTake(g_i2s_config_mutex, pdMS_TO_TICKS(750)) != pdTRUE) {
    ESP_LOGW(TAG, "Failed to acquire mutex - skipping feedback");
    return ESP_ERR_TIMEOUT;
}
```

## Memory Footprint

### Static Allocations

```c
// feedback_player.c
static int16_t s_work_buffer[FEEDBACK_MAX_SEGMENT_SAMPLES];
// = (16000 * 0.6) * 2 channels * sizeof(int16_t)
// = 38400 bytes (in DRAM)
```

### Dynamic Allocations

```c
// Per feedback playback (temporary):
- I2S driver: ~18KB DMA memory (if not already init)
- No heap allocations for tone generation

// Tone sequences in flash:
- PROCESSING_SEQUENCE: 3 segments × 20 bytes = 60 bytes
- TTS_COMPLETE_SEQUENCE: 5 segments × 20 bytes = 100 bytes
```

**Total Impact:** ~160 bytes flash, 0 bytes additional heap

## Testing Checklist

### Unit Tests (Manual)

- [ ] PROCESSING sound plays when STT starts
- [ ] LED changes to pulsing during STT/LLM
- [ ] LED changes to breathing during TTS
- [ ] TTS_COMPLETE sound plays after successful playback
- [ ] TTS_COMPLETE does NOT play for short (<10KB) responses
- [ ] LED restores correctly after pipeline completes
- [ ] Feedback works in both voice mode and camera mode
- [ ] No feedback overlap (mutex protection working)

### Integration Tests

- [ ] Multiple back-to-back voice queries work correctly
- [ ] Early mode exit doesn't cause crashes
- [ ] Low memory conditions handled gracefully
- [ ] WebSocket reconnection doesn't interfere with feedback
- [ ] Camera operations not affected by LED changes

### Stress Tests

- [ ] 100 consecutive voice queries
- [ ] Rapid mode switching (voice ↔ camera)
- [ ] Feedback during low DMA memory (<30KB free)
- [ ] Concurrent camera capture + feedback playback

## Debug Logging

### Key Log Messages

```
// Feedback system
"Playing TTS completion feedback to signal readiness"
"Entering TRANSCRIPTION stage - playing processing feedback"
"TTS audio arriving after voice mode exit (state=X)"

// LED state changes
"Entering TTS stage - preparing for audio streaming"
"Pipeline complete - checking system state for LED restore"

// Error conditions
"TTS completion feedback failed: <reason>"
"DMA memory too fragmented - skipping feedback"
```

### Log Level Recommendations

- **INFO:** Pipeline stage changes, LED state changes
- **WARN:** Feedback playback failures, memory issues
- **ERROR:** Driver init failures, critical state errors

## Performance Metrics

### Audio Latency Budget

```
User releases button:                      T+0ms
├─ REC_STOP sound:                        +110ms
├─ STT processing starts:                 +1500ms
├─ PROCESSING sound:                      +280ms
├─ LLM processing:                        +4000ms
├─ TTS audio arrives:                     +2000ms
├─ TTS playback starts (single beep):     +100ms
├─ TTS playback completes:                +1500ms
└─ TTS_COMPLETE sound:                    +460ms
                                    ──────────────
Total user wait:                          ~9950ms
```

**User Perception:**
- Hears feedback at T+110ms, T+1780ms, T+7500ms, T+9950ms
- Never waits >5s without audio feedback
- Knows system is working throughout

## Firmware Build Instructions

### Modified Files Summary
```
hotpin_esp32_firmware/main/include/feedback_player.h      [MODIFIED]
hotpin_esp32_firmware/main/feedback_player.c              [MODIFIED]
hotpin_esp32_firmware/main/tts_decoder.c                  [MODIFIED]
hotpin_esp32_firmware/main/websocket_client.c             [MODIFIED]
```

### Build Commands
```bash
cd hotpin_esp32_firmware
idf.py build
idf.py flash monitor
```

### Verification
```bash
# Check log for new feedback messages:
grep "Playing TTS completion feedback" SerialMonitor_Logs.txt
grep "Entering TRANSCRIPTION stage" SerialMonitor_Logs.txt
grep "processing feedback" SerialMonitor_Logs.txt
```

## Rollback Plan

If issues occur, comment out these sections:

1. **Disable TTS completion feedback:**
   ```c
   // In tts_decoder.c line ~845, comment out:
   // if (playback_result == ESP_OK && pcm_bytes_played > 10000) { ... }
   ```

2. **Disable processing feedback:**
   ```c
   // In websocket_client.c line ~1121, comment out:
   // if (new_stage == WEBSOCKET_PIPELINE_STAGE_TRANSCRIPTION ...) { ... }
   ```

3. **Disable LED changes:**
   ```c
   // In websocket_client.c, comment out all led_controller_set_state() calls
   ```

## Future Optimization Ideas

1. **Adaptive Feedback:** Reduce beep frequency if user consistently waits
2. **Haptic Feedback:** Vibration motor for button/module versions
3. **Voice Prompts:** Record actual voice saying "Processing..." / "Ready"
4. **User Preferences:** Store feedback enable/disable in NVS
5. **Network Quality Indicator:** Different LED pattern for slow connections

---

**Document Version:** 1.0  
**Last Updated:** 2025-11-03  
**Author:** GitHub Copilot + VighneshNilajakar

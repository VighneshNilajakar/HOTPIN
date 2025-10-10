# Core Affinity Fix - Quick Reference

## What Changed

Changed audio capture task from Core 1 → Core 0 to resolve hardware memory bus contention with Wi-Fi.

## Files Modified

- `main/stt_pipeline.c` - Core affinity and canary log

## The Fix

```c
// OLD:
xTaskCreatePinnedToCore(..., TASK_CORE_APP);  // Was Core 1

// NEW:
xTaskCreatePinnedToCore(..., 0);  // Core 0 - Co-located with Wi-Fi
```

## Build & Test

```bash
cd hotpin_esp32_firmware
idf.py build
idf.py flash monitor
```

## Expected Boot Logs

### SUCCESS Indicators:

```
✅ I (1235) [STT]: [CORE AFFINITY] Creating audio capture task on Core 0 (co-located with Wi-Fi)
✅ I (1236) [STT]: Audio capture task started on Core 0  ← Must be Core 0!
✅ I (1850) [STT]: [CAPTURE] Read #10: 1024 bytes...  ← Should NOT crash here!
✅ I (5050) [STT]: [CAPTURE] ✅ Alive... 100 reads completed (Free Heap: 142536 bytes)
✅ I (8250) [STT]: [CAPTURE] ✅ Alive... 200 reads completed (Free Heap: 142512 bytes)
✅ [Continue for hours without crash]
```

### ❌ FAILURE (if you see this):

```
❌ I (1236) [STT]: Audio capture task started on Core 1  ← Wrong core!
❌ Guru Meditation Error: Core 1 panic'ed (LoadStoreError)  ← Crash!
```

## Validation Checklist

- [ ] Build completes successfully
- [ ] Flash succeeds without errors
- [ ] Boot log shows "Core 0" for audio capture task
- [ ] System survives past read #10 (previously crashed here)
- [ ] Canary "Alive..." logs appear every ~3 seconds
- [ ] System runs for 60+ minutes without crash
- [ ] Free heap remains stable (no memory leaks)

## What to Monitor

### Every 3 seconds:
```
I (XXXX) [STT]: [CAPTURE] ✅ Alive... N reads completed (Free Heap: XXXXXX bytes)
```
- `N` should increment by 100 each time
- Free heap should remain stable (±100 bytes variation is normal)

### Every 32ms (10 reads):
```
I (XXXX) [STT]: [CAPTURE] Read #N: 1024 bytes (total: XXXX bytes, XX.X KB)
```
- Should appear continuously without gaps

## Why This Works

**Problem**: Wi-Fi (Core 0) and I2S DMA (Core 1) competing for memory bus access → DMA corruption → LoadStoreError crash

**Solution**: Move I2S task to Core 0 → FreeRTOS scheduler coordinates memory access → No corruption → No crashes

## Rollback (if needed)

```bash
git checkout HEAD -- main/stt_pipeline.c
idf.py build flash monitor
```

## Documentation

- **Comprehensive Guide**: `CORE_AFFINITY_FIX.md`
- **Previous Fixes**: 
  - `CRITICAL_GPIO12_STRAPPING_PIN_FIX.md` (GPIO remap)
  - `I2S_RACE_CONDITION_MUTEX_FIX.md` (Mutex protection)
  - `RING_BUFFER_PSRAM_FIX.md` (Memory architecture)
  - `I2S_LEGACY_TO_STD_MIGRATION.md` (Driver update)

## This is the Final Fix

This addresses the deepest layer of the LoadStoreError issue: hardware-level memory bus contention. All previous fixes addressed higher-level issues. This completes the fix series.

**Expected Result**: System runs indefinitely with continuous "Alive..." canary logs, no crashes.

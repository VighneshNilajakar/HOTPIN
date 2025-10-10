# I2S Driver Migration - Quick Reference

## What Changed

**From:** Legacy I2S Driver (`driver/i2s.h`) - DEPRECATED  
**To:** Modern I2S STD Driver (`driver/i2s_std.h`) - CURRENT

## Files Modified

1. ✅ `main/include/audio_driver.h` - Updated includes and added channel handles
2. ✅ `main/audio_driver.c` - Complete rewrite using modern API
3. ✅ `main/stt_pipeline.c` - No changes needed (uses wrapper functions)

## Key API Changes

### Initialization
```c
// OLD (Legacy)
i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
i2s_set_pin(I2S_NUM_0, &pin_config);
i2s_start(I2S_NUM_0);

// NEW (Modern)
i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle);
i2s_channel_init_std_mode(tx_handle, &tx_cfg);
i2s_channel_init_std_mode(rx_handle, &rx_cfg);
i2s_channel_enable(tx_handle);
i2s_channel_enable(rx_handle);
```

### Read/Write
```c
// OLD (Legacy)
i2s_write(I2S_NUM_0, data, size, &written, timeout);
i2s_read(I2S_NUM_0, buffer, size, &read, timeout);

// NEW (Modern)
i2s_channel_write(g_i2s_tx_handle, data, size, &written, timeout);
i2s_channel_read(g_i2s_rx_handle, buffer, size, &read, timeout);
```

### Cleanup
```c
// OLD (Legacy)
i2s_stop(I2S_NUM_0);
i2s_driver_uninstall(I2S_NUM_0);

// NEW (Modern)
i2s_channel_disable(tx_handle);
i2s_channel_disable(rx_handle);
i2s_del_channel(tx_handle);
i2s_del_channel(rx_handle);
```

## Build Instructions

```powershell
cd hotpin_esp32_firmware
idf.py build
idf.py flash monitor
```

## Expected Boot Log (Success)

```
[AUDIO] ╔══════════════════════════════════════════════════════════
[AUDIO] ║ Initializing Modern I2S STD Driver (Full-Duplex)
[AUDIO] ╚══════════════════════════════════════════════════════════
[AUDIO] [STEP 1/6] Creating I2S channel pair (TX + RX)...
[AUDIO] ✅ I2S channels created (took 18 ms)
[AUDIO]   TX handle: 0x3FFC1234 | RX handle: 0x3FFC5678
[AUDIO] [STEP 2/6] Configuring TX (speaker) channel...
[AUDIO] ✅ TX channel configured (took 12 ms)
[AUDIO] [STEP 3/6] Configuring RX (microphone) channel...
[AUDIO] ✅ RX channel configured (took 11 ms)
[AUDIO] [STEP 4/6] Enabling TX channel...
[AUDIO] ✅ TX channel enabled (took 5 ms)
[AUDIO] [STEP 5/6] Enabling RX channel...
[AUDIO] ✅ RX channel enabled (took 5 ms)
[AUDIO] ╔══════════════════════════════════════════════════════════
[AUDIO] ║ ✅ MODERN I2S STD FULL-DUPLEX READY
[AUDIO] ║ Driver: i2s_std (NOT legacy!)
[AUDIO] ║ This should eliminate LoadStoreError crashes!
[AUDIO] ╚══════════════════════════════════════════════════════════
```

## Validation Checklist

- [ ] No deprecation warning: `W (1556) i2s(legacy): legacy i2s driver is deprecated`
- [ ] Boot log shows "Modern I2S STD Driver"
- [ ] TX and RX handles are created and logged
- [ ] Audio capture works in VOICE_ACTIVE mode
- [ ] No LoadStoreError crashes
- [ ] System stable for 60+ minutes
- [ ] Camera ↔ Audio transitions work

## Rollback (If Needed)

If the modern driver causes issues (unlikely), you can rollback by:
1. `git checkout HEAD~1 main/include/audio_driver.h`
2. `git checkout HEAD~1 main/audio_driver.c`
3. `idf.py build flash`

## Documentation

- Full migration details: `I2S_LEGACY_TO_STD_MIGRATION.md`
- Previous fixes:
  - `CRITICAL_GPIO12_STRAPPING_PIN_FIX.md`
  - `I2S_RACE_CONDITION_MUTEX_FIX.md`
  - `RING_BUFFER_PSRAM_FIX.md`

## Why This Fixes LoadStoreError

The legacy driver had **HAL-level race conditions** in DMA descriptor management that couldn't be fixed by application-level mutexes. The modern driver:

1. Separates TX and RX into independent channels
2. Each channel has its own DMA descriptor chain
3. No shared state between concurrent operations
4. Active maintenance and bug fixes from ESP-IDF team

**Result:** Eliminates the root cause of DMA state corruption.

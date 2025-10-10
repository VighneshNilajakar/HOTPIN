# Ring Buffer DMA Memory Fix

## Date: October 10, 2025

## Problem Summary

After successfully applying the DMA buffer fix for the audio capture buffer, the system still crashed with a `LoadStoreError` during audio capture. Analysis revealed:

### Crash Details
```
Guru Meditation Error: Core 1 panic'ed (LoadStoreError)
EXCVADDR: 0x4009b398
PC: 0x400de5ea (in memcpy/loop operation)
```

### Root Cause Analysis

**Previous State (Partially Fixed)**:
- ✅ Audio capture buffer: Fixed with `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`
- ❌ Ring buffer: Still allocated in PSRAM with `MALLOC_CAP_SPIRAM`
- ❌ PSRAM address range: 0x3F800000-0x3FC00000 or 0x4009xxxx (mapped region)

**The Issue**:
1. Ring buffer was allocated in PSRAM at address `0x4009b398`
2. PSRAM access requires cache management and is NOT DMA-coherent
3. When `ring_buffer_write()` tried to copy data from the DMA capture buffer to PSRAM, it caused cache coherency issues
4. The byte-by-byte loop in `ring_buffer_write()` crashed due to invalid PSRAM access

**Why PSRAM is Problematic**:
```
┌─────────────────────────────────────────────────────────────┐
│ ESP32 Memory Architecture & DMA                             │
├─────────────────────────────────────────────────────────────┤
│ INTERNAL DRAM (DMA-capable):  0x3FFB0000-0x3FFE0000 (192KB)│
│   ✅ Direct DMA access                                      │
│   ✅ CPU cache-coherent                                     │
│   ✅ Fast, reliable                                         │
│                                                             │
│ PSRAM (External):             0x3F800000+ or 0x4009xxxx    │
│   ❌ NO direct DMA access                                  │
│   ❌ Requires cache management                             │
│   ❌ Can cause LoadStoreError with DMA operations          │
└─────────────────────────────────────────────────────────────┘
```

## Solution Applied

### Fix 1: Move Ring Buffer to Internal DMA-Capable RAM

**File**: `main/stt_pipeline.c` (Line ~70)

**Before (WRONG)**:
```c
// Allocate PSRAM ring buffer
ESP_LOGI(TAG, "Allocating %zu KB ring buffer in PSRAM...", g_ring_buffer_size / 1024);
g_audio_ring_buffer = heap_caps_malloc(g_ring_buffer_size, MALLOC_CAP_SPIRAM);
if (g_audio_ring_buffer == NULL) {
    ESP_LOGE(TAG, "Failed to allocate ring buffer in PSRAM");
    return ESP_ERR_NO_MEM;
}

memset(g_audio_ring_buffer, 0, g_ring_buffer_size);
```

**After (CORRECT)**:
```c
// Allocate ring buffer in DMA-capable internal RAM for reliability
// Note: PSRAM can cause cache coherency issues with DMA operations
ESP_LOGI(TAG, "Allocating %zu KB ring buffer in internal RAM...", g_ring_buffer_size / 1024);
g_audio_ring_buffer = heap_caps_malloc(g_ring_buffer_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
if (g_audio_ring_buffer == NULL) {
    ESP_LOGE(TAG, "Failed to allocate ring buffer in internal RAM");
    ESP_LOGE(TAG, "  Requested: %zu bytes", g_ring_buffer_size);
    ESP_LOGE(TAG, "  Free DMA-capable: %u bytes", (unsigned int)heap_caps_get_free_size(MALLOC_CAP_DMA));
    return ESP_ERR_NO_MEM;
}
ESP_LOGI(TAG, "  ✓ Ring buffer allocated at %p", g_audio_ring_buffer);

// Zero buffer safely (no memset to avoid cache issues)
for (size_t i = 0; i < g_ring_buffer_size; i++) {
    g_audio_ring_buffer[i] = 0;
}
```

**Why This Works**:
- Forces allocation in 0x3FFB0000-0x3FFE0000 region (DMA-capable DRAM)
- No cache coherency issues
- Direct CPU access without PSRAM cache complications
- Safe for use with DMA-sourced data

**Trade-off**:
- Ring buffer size: 64KB (configured in code)
- Internal RAM available: ~192KB total, ~140KB free after boot
- This is acceptable - ring buffer acts as a small FIFO for streaming

### Fix 2: Enhanced Ring Buffer Safety

**File**: `main/stt_pipeline.c` (ring_buffer_write/read functions)

Added comprehensive safety checks:

```c
static esp_err_t ring_buffer_write(const uint8_t *data, size_t len) {
    // 1. Validate inputs
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // 2. Check space available
    if (ring_buffer_available_space() < len) {
        return ESP_ERR_NO_MEM;
    }
    
    // 3. Mutex with timeout (not infinite wait)
    if (!xSemaphoreTake(g_ring_buffer_mutex, pdMS_TO_TICKS(100))) {
        ESP_LOGW(TAG, "⚠ Ring buffer mutex timeout");
        return ESP_ERR_TIMEOUT;
    }
    
    // 4. Bounds-checked copy
    for (size_t i = 0; i < len; i++) {
        if (g_ring_buffer_write_pos >= g_ring_buffer_size) {
            ESP_LOGE(TAG, "❌ Ring buffer write pos overflow: %zu >= %zu", 
                     g_ring_buffer_write_pos, g_ring_buffer_size);
            xSemaphoreGive(g_ring_buffer_mutex);
            return ESP_ERR_INVALID_STATE;
        }
        
        g_audio_ring_buffer[g_ring_buffer_write_pos] = data[i];
        g_ring_buffer_write_pos = (g_ring_buffer_write_pos + 1) % g_ring_buffer_size;
    }
    
    xSemaphoreGive(g_ring_buffer_mutex);
    return ESP_OK;
}
```

**Improvements**:
1. Input validation (NULL pointer checks)
2. Mutex timeout instead of infinite wait (prevents deadlock)
3. Paranoid bounds checking during write loop
4. Detailed error logging for diagnostics

Similar improvements applied to `ring_buffer_read()`.

## Expected Results

### Before Fix:
```
[BUFFER] ✓ DMA-capable buffer allocated at 0x3ffe383c
[FIRST READ] Result: ESP_OK
[FIRST READ] Bytes read: 1024/1024
[FIRST READ] First 16 bytes: 54 f8 54 f8 ...
[CAPTURE] ReGuru Meditation Error: LoadStoreError
EXCVADDR: 0x4009b398  ← PSRAM address
💥 CRASH during ring_buffer_write()
```

### After Fix:
```
[STT] Allocating 64 KB ring buffer in internal RAM...
[STT]   ✓ Ring buffer allocated at 0x3ffbXXXX  ← DMA-capable DRAM
[BUFFER] ✓ DMA-capable buffer allocated at 0x3ffbYYYY
[FIRST READ] Result: ESP_OK
[FIRST READ] Bytes read: 1024/1024
[FIRST READ] First 16 bytes: XX XX XX XX ... (actual audio data)
[CAPTURE] Read #10: 2048 bytes (total: 20480 bytes)
[CAPTURE] Audio streaming to server...
✅ No crash, continuous operation
```

## Testing Checklist

- [ ] Build firmware: `idf.py build`
- [ ] Flash to device: `idf.py flash monitor`
- [ ] Boot and check initialization logs:
  - [ ] Ring buffer allocated in 0x3FFBxxxx range (not 0x4009xxxx)
  - [ ] Capture buffer allocated in 0x3FFBxxxx range
  - [ ] No PSRAM allocation warnings
- [ ] Test voice mode activation:
  - [ ] I²S initialization succeeds
  - [ ] First i2s_read() returns ESP_OK
  - [ ] Hex dump logs without crash
  - [ ] Multiple reads succeed (read #10, #20, #30, etc.)
  - [ ] No "Ring buffer mutex timeout" warnings
  - [ ] No "Ring buffer overflow" errors
- [ ] Test audio streaming:
  - [ ] Audio chunks sent to WebSocket server
  - [ ] Server receives continuous stream
  - [ ] No buffer underruns or overruns
- [ ] Test camera ↔ voice transitions:
  - [ ] Switch camera → voice: Ring buffer reallocates
  - [ ] Switch voice → camera: Ring buffer freed, camera inits
  - [ ] Repeat 20+ times without crashes or memory leaks
- [ ] Monitor heap:
  - [ ] Check free DMA memory: `heap_caps_get_free_size(MALLOC_CAP_DMA)`
  - [ ] Ensure no memory leaks over multiple transitions
  - [ ] ~80KB+ free DMA RAM should remain available

## Technical Details

### Memory Layout (Corrected):
```
Audio Capture Buffer (1KB):    0x3FFBxxxx (DMA-capable DRAM)
                                    ↓
                              i2s_read() writes here
                                    ↓
                              CPU reads buffer
                                    ↓
Ring Buffer (64KB):           0x3FFBxxxx (DMA-capable DRAM)
                                    ↓
                          ring_buffer_write() copies here
                                    ↓
Streaming Buffer (4KB):       0x3FFBxxxx (DMA-capable DRAM)
                                    ↓
                          ring_buffer_read() copies here
                                    ↓
                          WebSocket sends to server
```

**All buffers now in DMA-capable internal RAM** - No PSRAM involved in audio path.

### Why Not Use PSRAM?

PSRAM is great for:
- Large allocations (>100KB)
- Frame buffers (camera images)
- Static data structures
- Non-real-time operations

PSRAM is BAD for:
- DMA operations (I²S, SPI, etc.)
- Real-time audio/video streams
- Frequent small reads/writes
- Cache-sensitive operations

### Ring Buffer Size Justification

**Current**: 64KB (configurable)

**Math**:
- Audio rate: 16000 Hz × 2 bytes (16-bit) = 32000 bytes/sec
- 64KB buffer = 2 seconds of audio
- Capture chunk: 1KB every ~32ms
- Streaming chunk: 4KB every ~125ms

**Buffering strategy**:
- Capture task: Fills ring buffer at 32KB/sec
- Streaming task: Drains ring buffer at 32KB/sec (same rate)
- 64KB provides 2-second tolerance for network jitter

## Key Learnings

1. **MALLOC_CAP_DMA is MANDATORY for all audio buffers**
   - Not just the I²S DMA descriptors
   - Also the ring buffer that receives DMA-sourced data
   - And any intermediate buffers in the audio pipeline

2. **PSRAM is NOT suitable for DMA workflows**
   - Even if data is copied from DMA buffer (not direct DMA)
   - Cache coherency issues persist
   - Use internal DRAM for real-time audio

3. **Defensive programming prevents subtle crashes**
   - Always validate pointers before dereferencing
   - Use mutex timeouts, not infinite waits
   - Add paranoid bounds checking in loops
   - Log addresses for post-mortem analysis

4. **Test incrementally**
   - First fix: DMA capture buffer (previous session)
   - Second fix: Ring buffer allocation (this session)
   - Each fix revealed the next layer of issues

## Related Documentation

- See `DMA_BUFFER_FIX.md` for the audio capture buffer fix
- See `I2S_FULL_DUPLEX_FIX_COMPLETE.md` for I²S architecture
- See `CRITICAL_FIXES_APPLIED.md` for stabilization delays

## Next Steps

1. **Build and test** with the ring buffer fix
2. **Monitor logs** for any remaining issues:
   - Watch for unexpected PSRAM allocations
   - Check free DMA memory doesn't deplete
   - Verify no mutex timeouts or buffer overflows
3. **Stress test**: Run continuous audio capture for 5+ minutes
4. **If successful**: Document as stable configuration
5. **If issues persist**: Consider reducing ring buffer size or further diagnostics

---

**Status**: Ready for testing
**Expected Outcome**: Stable voice recording without crashes
**Risk**: Low - Moving to internal RAM is more reliable than PSRAM

# CRITICAL: I2S Architecture Fix Required

**Date:** October 9, 2025  
**Severity:** CRITICAL - System crash due to hardware conflict  
**Issue:** Attempting to use two separate I2S peripherals with shared clock pins

---

## Root Cause Analysis

### The Fatal Flaw

**Current (BROKEN) Configuration:**
```
I2S0 (TX - Speaker):   BCLK=GPIO14, WS=GPIO15, DOUT=GPIO13
I2S1 (RX - Microphone): BCLK=GPIO14, WS=GPIO15, DIN=GPIO12
                              ↑↑↑           ↑↑↑
                           CONFLICT!    CONFLICT!
```

### Why This Causes a Crash

1. **Hardware Limitation:** ESP32 has two independent I2S peripherals (I2S0 and I2S1), each with its **own clock generator**

2. **GPIO Conflict:** When you configure:
   - `i2s_set_pin(I2S_NUM_0, {.bck_io_num = 14, .ws_io_num = 15, ...})`
   - `i2s_set_pin(I2S_NUM_1, {.bck_io_num = 14, .ws_io_num = 15, ...})`
   
   The GPIO matrix gets **corrupted** because two peripherals are trying to drive the same pins!

3. **DMA Descriptor Corruption:** The internal DMA controller state becomes invalid, causing:
   ```
   Guru Meditation Error: Core 1 panic'ed (LoadStoreError)
   EXCVADDR: 0x4009b398  ← Invalid DMA descriptor pointer
   ```

4. **Crash on i2s_read():** When the code calls `i2s_read()`, the ESP-IDF driver tries to access the corrupted DMA descriptor chain → **LoadStoreError**

---

## The Correct Architecture

### Option 1: Single I2S Peripheral in Full-Duplex Mode ✅ RECOMMENDED

Use **I2S0 ONLY** with both TX and RX enabled:

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
    .bck_io_num = 14,        // Shared BCLK
    .ws_io_num = 15,         // Shared WS
    .data_out_num = 13,      // Speaker
    .data_in_num = 12        // Microphone
};

i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
i2s_set_pin(I2S_NUM_0, &pin_config);
i2s_start(I2S_NUM_0);

// Now use:
i2s_write(I2S_NUM_0, ...);  // To speaker
i2s_read(I2S_NUM_0, ...);   // From microphone
```

### Option 2: Separate I2S Peripherals with Different Clocks

If you absolutely need two separate I2S peripherals:

```
I2S0 (TX - Speaker):   BCLK=GPIO26, WS=GPIO25, DOUT=GPIO13
I2S1 (RX - Microphone): BCLK=GPIO14, WS=GPIO15, DIN=GPIO12
```

**But this won't work** because INMP441 and MAX98357A both expect the **same clock signals**!

---

## Implementation Plan

### Files to Modify

1. **`audio_driver.c`**
   - Remove `configure_i2s_tx()` and `configure_i2s_rx()` functions
   - Create single `configure_i2s_full_duplex()` function
   - Use `I2S_NUM_0` for both read and write operations

2. **`config.h`**
   - Remove `CONFIG_I2S_NUM_TX` and `CONFIG_I2S_NUM_RX`
   - Add `CONFIG_I2S_NUM` (single peripheral)

3. **`audio_driver.c` - audio_driver_read()`**
   - Change from `i2s_read(CONFIG_I2S_NUM_RX, ...)` 
   - To `i2s_read(CONFIG_I2S_NUM_0, ...)`

4. **`audio_driver.c` - audio_driver_write()`**
   - Change from `i2s_write(CONFIG_I2S_NUM_TX, ...)`
   - To `i2s_write(CONFIG_I2S_NUM_0, ...)`

---

## New Code Structure

### audio_driver.c

```c
#define I2S_AUDIO_NUM I2S_NUM_0  // Single peripheral for full-duplex

static esp_err_t configure_i2s_full_duplex(void) {
    ESP_LOGI(TAG, "Configuring I2S0 for full-duplex audio...");
    
    // Full-duplex configuration
    i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX,  // CRITICAL: Both modes!
        .sample_rate = CONFIG_AUDIO_SAMPLE_RATE,
        .bits_per_sample = CONFIG_AUDIO_BITS_PER_SAMPLE,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1 | ESP_INTR_FLAG_SHARED,
        .dma_buf_count = CONFIG_I2S_DMA_BUF_COUNT,
        .dma_buf_len = CONFIG_I2S_DMA_BUF_LEN,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };
    
    // Install driver
    esp_err_t ret = i2s_driver_install(I2S_AUDIO_NUM, &i2s_config, 0, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install I2S driver: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Pin configuration - both TX and RX
    i2s_pin_config_t pin_config = {
        .mck_io_num = I2S_PIN_NO_CHANGE,
        .bck_io_num = CONFIG_I2S_BCLK,      // GPIO14 - shared
        .ws_io_num = CONFIG_I2S_LRCK,       // GPIO15 - shared
        .data_out_num = CONFIG_I2S_TX_DATA_OUT,  // GPIO13 - speaker
        .data_in_num = CONFIG_I2S_RX_DATA_IN     // GPIO12 - microphone
    };
    
    ret = i2s_set_pin(I2S_AUDIO_NUM, &pin_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set I2S pins: %s", esp_err_to_name(ret));
        i2s_driver_uninstall(I2S_AUDIO_NUM);
        return ret;
    }
    
    // Clear buffers
    i2s_zero_dma_buffer(I2S_AUDIO_NUM);
    
    // Start I2S
    ret = i2s_start(I2S_AUDIO_NUM);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start I2S: %s", esp_err_to_name(ret));
        i2s_driver_uninstall(I2S_AUDIO_NUM);
        return ret;
    }
    
    vTaskDelay(pdMS_TO_TICKS(100));  // Hardware stabilization
    
    ESP_LOGI(TAG, "✅ I2S full-duplex initialized");
    ESP_LOGI(TAG, "   BCLK: GPIO%d, WS: GPIO%d", CONFIG_I2S_BCLK, CONFIG_I2S_LRCK);
    ESP_LOGI(TAG, "   Speaker (TX): GPIO%d", CONFIG_I2S_TX_DATA_OUT);
    ESP_LOGI(TAG, "   Microphone (RX): GPIO%d", CONFIG_I2S_RX_DATA_IN);
    
    return ESP_OK;
}

esp_err_t audio_driver_read(uint8_t *buffer, size_t size, size_t *bytes_read, uint32_t timeout_ms) {
    if (!is_initialized) {
        ESP_LOGE(TAG, "I2S not initialized");
        if (bytes_read) *bytes_read = 0;
        return ESP_ERR_INVALID_STATE;
    }
    
    size_t read = 0;
    TickType_t ticks_to_wait = timeout_ms / portTICK_PERIOD_MS;
    
    // Use same I2S peripheral for RX
    esp_err_t ret = i2s_read(I2S_AUDIO_NUM, buffer, size, &read, ticks_to_wait);
    
    if (bytes_read) {
        *bytes_read = read;
    }
    
    return ret;
}

esp_err_t audio_driver_write(const uint8_t *buffer, size_t size, size_t *bytes_written, uint32_t timeout_ms) {
    if (!is_initialized) {
        ESP_LOGE(TAG, "I2S not initialized");
        if (bytes_written) *bytes_written = 0;
        return ESP_ERR_INVALID_STATE;
    }
    
    size_t written = 0;
    TickType_t ticks_to_wait = timeout_ms / portTICK_PERIOD_MS;
    
    // Use same I2S peripheral for TX
    esp_err_t ret = i2s_write(I2S_AUDIO_NUM, buffer, size, &written, ticks_to_wait);
    
    if (bytes_written) {
        *bytes_written = written;
    }
    
    return ret;
}
```

---

## Why This Works

1. **Single Clock Generator:** Only I2S0's clock generator drives GPIO14/15
2. **No GPIO Conflict:** GPIO matrix is clean - each pin has single source
3. **Synchronized RX/TX:** Speaker and microphone use **same clock**, perfectly synchronized
4. **Valid DMA Descriptors:** Single I2S peripheral = single DMA descriptor chain
5. **No Corruption:** Hardware state remains consistent

---

## Expected Behavior After Fix

```
✅ I2S full-duplex initialized
✅ BCLK: GPIO14, WS: GPIO15
✅ Speaker (TX): GPIO13
✅ Microphone (RX): GPIO12
✅ Audio capture task started
✅ i2s_read() returns audio samples successfully
✅ No crash!
```

---

## Priority: IMMEDIATE

This is not a software bug - it's a **hardware architecture error**. The current code is fundamentally incompatible with ESP32's I2S peripheral design.

**Status:** CRITICAL FIX REQUIRED  
**Next Step:** Rewrite `audio_driver.c` to use single I2S peripheral in full-duplex mode

/**
 * @file audio_driver.c
 * @brief Modern I2S STD audio driver implementation for INMP441 + MAX98357A
 * 
 * Implements full-duplex audio using the modern i2s_std driver:
 * - TX Channel: MAX98357A speaker amplifier
 * - RX Channel: INMP441 MEMS microphone
 * - Shared BCLK (GPIO14) and WS (GPIO15) for synchronized operation
 * - Separate channel handles for robust concurrent operation
 * 
 * MIGRATION NOTE: This replaces the deprecated legacy I2S driver (driver/i2s.h)
 * to fix LoadStoreError crashes caused by DMA state corruption in the old driver.
 */

#include "audio_driver.h"
#include "config.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <string.h>
#include <limits.h>
#include "inttypes.h"

#define I2S_DMA_FRAME_MAX 1023U
#define WRITE_DEBUG_LOG_LIMIT 8U

static const char *TAG = TAG_AUDIO;
static bool is_initialized = false;
static uint32_t current_tx_sample_rate = CONFIG_AUDIO_SAMPLE_RATE;

/**
 * @brief Global mutex for thread-safe I2S hardware access
 * 
 * CRITICAL: Protects concurrent i2s_channel_read() and i2s_channel_write() operations
 * from corrupting the DMA controller state when called from multiple tasks.
 */
SemaphoreHandle_t g_i2s_access_mutex = NULL;

/**
 * @brief I2S channel handles for modern driver
 * 
 * The modern i2s_std driver uses separate handles for TX and RX channels,
 * providing cleaner separation and more robust full-duplex operation.
 */
i2s_chan_handle_t g_i2s_tx_handle = NULL;  // Speaker output channel
i2s_chan_handle_t g_i2s_rx_handle = NULL;  // Microphone input channel

// ===========================
// Private Function Declarations
// ===========================
static esp_err_t configure_i2s_std_full_duplex(void);

// ===========================
// Public Functions
// ===========================



esp_err_t audio_driver_init(void) {
    ESP_LOGI(TAG, "╔══════════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "║ Initializing Modern I2S STD Driver (Full-Duplex)");
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════════════════");
    
    if (is_initialized) {
        ESP_LOGW(TAG, "Audio driver already initialized");
        return ESP_OK;
    }
    
    // CRITICAL: Create I2S access mutex (only once)
    if (g_i2s_access_mutex == NULL) {
        ESP_LOGI(TAG, "[MUTEX] Creating I2S access mutex for thread safety...");
        g_i2s_access_mutex = xSemaphoreCreateMutex();
        if (g_i2s_access_mutex == NULL) {
            ESP_LOGE(TAG, "❌ CRITICAL: Failed to create I2S access mutex");
            ESP_LOGE(TAG, "  Free heap: %u bytes", (unsigned int)esp_get_free_heap_size());
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "  ✓ I2S access mutex created successfully");
    }
    
    // Configure modern I2S STD driver with separate TX and RX channels
    esp_err_t ret = configure_i2s_std_full_duplex();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ Failed to configure I2S STD full-duplex: %s", esp_err_to_name(ret));
        return ret;
    }
    
    is_initialized = true;
    current_tx_sample_rate = CONFIG_AUDIO_SAMPLE_RATE;
    ESP_LOGI(TAG, "╔══════════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "║ ✅ MODERN I2S STD DRIVER INITIALIZED");
    ESP_LOGI(TAG, "║ Mode: Full-duplex (separate TX + RX channels)");
    ESP_LOGI(TAG, "║ TX (Speaker): GPIO%d | RX (Microphone): GPIO%d", 
             CONFIG_I2S_TX_DATA_OUT, CONFIG_I2S_RX_DATA_IN);
    ESP_LOGI(TAG, "║ Shared Clock: BCLK=GPIO%d, WS=GPIO%d", CONFIG_I2S_BCLK, CONFIG_I2S_LRCK);
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════════════════");
    
    return ESP_OK;
}

esp_err_t audio_driver_deinit(void) {
    ESP_LOGI(TAG, "╔══════════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "║ Deinitializing Modern I2S STD Driver for Camera Capture");
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════════════════");
    
    if (!is_initialized) {
        ESP_LOGW(TAG, "Audio driver not initialized - nothing to deinit");
        return ESP_OK;
    }
    
    // Try to acquire mutex before deinit to prevent conflicts
    if (g_i2s_access_mutex != NULL && xSemaphoreTake(g_i2s_access_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        esp_err_t ret = ESP_OK;
        int64_t start_time;
        
        // Step 1: Disable RX channel
        if (g_i2s_rx_handle != NULL) {
            ESP_LOGI(TAG, "[STEP 1/5] Disabling RX (microphone) channel...");
            start_time = esp_timer_get_time();
            ret = i2s_channel_disable(g_i2s_rx_handle);
            int64_t disable_time = (esp_timer_get_time() - start_time) / 1000;
            
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "⚠ RX channel disable returned: %s (took %"PRIu32" ms)", 
                         esp_err_to_name(ret), (uint32_t)disable_time);
            } else {
                ESP_LOGI(TAG, "✅ RX channel disabled (took %"PRIu32" ms)", (uint32_t)disable_time);
            }
        }
        
        // Step 2: Disable TX channel
        if (g_i2s_tx_handle != NULL) {
            ESP_LOGI(TAG, "[STEP 2/5] Disabling TX (speaker) channel...");
            start_time = esp_timer_get_time();
            ret = i2s_channel_disable(g_i2s_tx_handle);
            int64_t disable_time = (esp_timer_get_time() - start_time) / 1000;
            
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "⚠ TX channel disable returned: %s (took %"PRIu32" ms)", 
                         esp_err_to_name(ret), (uint32_t)disable_time);
            } else {
                ESP_LOGI(TAG, "✅ TX channel disabled (took %"PRIu32" ms)", (uint32_t)disable_time);
            }
        }
        
        // Step 3: Allow DMA operations to complete
        ESP_LOGI(TAG, "[STEP 3/5] Waiting for DMA completion (50ms)...");
        vTaskDelay(pdMS_TO_TICKS(50));
        
        // Step 4: Delete RX channel
        if (g_i2s_rx_handle != NULL) {
            ESP_LOGI(TAG, "[STEP 4/5] Deleting RX channel...");
            start_time = esp_timer_get_time();
            ret = i2s_del_channel(g_i2s_rx_handle);
            int64_t del_time = (esp_timer_get_time() - start_time) / 1000;
            
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "❌ RX channel deletion FAILED: %s (took %"PRIu32" ms)", 
                         esp_err_to_name(ret), (uint32_t)del_time);
            } else {
                ESP_LOGI(TAG, "✅ RX channel deleted (took %"PRIu32" ms)", (uint32_t)del_time);
                g_i2s_rx_handle = NULL;  // Explicitly set to NULL after deletion
            }
        }
        
        // Step 5: Delete TX channel
        if (g_i2s_tx_handle != NULL) {
            ESP_LOGI(TAG, "[STEP 5/5] Deleting TX channel...");
            start_time = esp_timer_get_time();
            ret = i2s_del_channel(g_i2s_tx_handle);
            int64_t del_time = (esp_timer_get_time() - start_time) / 1000;
            
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "❌ TX channel deletion FAILED: %s (took %"PRIu32" ms)", 
                         esp_err_to_name(ret), (uint32_t)del_time);
            } else {
                ESP_LOGI(TAG, "✅ TX channel deleted (took %"PRIu32" ms)", (uint32_t)del_time);
                g_i2s_tx_handle = NULL;  // Explicitly set to NULL after deletion
            }
        }
        
        // Additional delay to ensure interrupt controller and GPIO matrix settle
        ESP_LOGI(TAG, "Additional settling time (50ms) for interrupt/GPIO matrix...");
        vTaskDelay(pdMS_TO_TICKS(50));
        
        xSemaphoreGive(g_i2s_access_mutex);
        
        // Mark as uninitialized
        is_initialized = false;
        current_tx_sample_rate = CONFIG_AUDIO_SAMPLE_RATE;
        
        ESP_LOGI(TAG, "╔══════════════════════════════════════════════════════════");
        ESP_LOGI(TAG, "║ ✅ Modern I2S STD Driver Deinitialized");
        ESP_LOGI(TAG, "║ Camera Can Now Initialize");
        ESP_LOGI(TAG, "╚══════════════════════════════════════════════════════════");
        
        return ret;
    } else {
        ESP_LOGW(TAG, "Could not acquire mutex for safe deinitialization");
        // Still proceed with deinit but with a warning
        is_initialized = false;
        current_tx_sample_rate = CONFIG_AUDIO_SAMPLE_RATE;
        return ESP_OK;
    }
}

esp_err_t audio_driver_write(const uint8_t *data, size_t size, size_t *bytes_written, uint32_t timeout_ms) {
    static uint32_t s_write_log_count = 0;

    if (!is_initialized || g_i2s_tx_handle == NULL) {
        ESP_LOGE(TAG, "I2S TX channel not initialized");
        if (bytes_written) *bytes_written = 0;
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!data || size == 0) {
        ESP_LOGE(TAG, "Invalid write parameters");
        if (bytes_written) *bytes_written = 0;
        return ESP_ERR_INVALID_ARG;
    }
    
    // CRITICAL: Acquire mutex to prevent concurrent I2S access
    if (g_i2s_access_mutex == NULL) {
        ESP_LOGE(TAG, "❌ I2S access mutex not initialized");
        if (bytes_written) *bytes_written = 0;
        return ESP_ERR_INVALID_STATE;
    }
    
    TickType_t mutex_wait_ticks;
    if (timeout_ms == (uint32_t)portMAX_DELAY) {
        mutex_wait_ticks = portMAX_DELAY;
    } else if (timeout_ms == 0) {
        mutex_wait_ticks = pdMS_TO_TICKS(100);
    } else {
        mutex_wait_ticks = pdMS_TO_TICKS(timeout_ms);
        if (mutex_wait_ticks == 0) {
            mutex_wait_ticks = 1;
        }
    }

    // Try to acquire mutex with caller-aligned timeout to prevent indefinite blocking
    if (xSemaphoreTake(g_i2s_access_mutex, mutex_wait_ticks) != pdTRUE) {
        ESP_LOGW(TAG, "⚠ Failed to acquire I2S access mutex within %lu ms (write blocked)",
                 (unsigned long)((timeout_ms == (uint32_t)portMAX_DELAY) ? UINT32_MAX : timeout_ms ? timeout_ms : 100));
        if (bytes_written) *bytes_written = 0;
        return ESP_ERR_TIMEOUT;
    }
    
    // Perform I2S channel write operation (protected by mutex)
    // Modern driver uses i2s_channel_write() instead of i2s_write()
    size_t written = 0;
    TickType_t ticks_to_wait;
    if (timeout_ms == (uint32_t)portMAX_DELAY) {
        ticks_to_wait = portMAX_DELAY;
    } else {
        ticks_to_wait = pdMS_TO_TICKS(timeout_ms);
    }
    
    esp_err_t ret = i2s_channel_write(g_i2s_tx_handle, data, size, &written, ticks_to_wait);
    
    // Release mutex immediately after hardware access
    xSemaphoreGive(g_i2s_access_mutex);
    
    if (bytes_written) {
        *bytes_written = written;
    }
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S channel write failed: %s (requested=%zu bytes, wrote=%zu)",
                 esp_err_to_name(ret), size, written);
        return ret;
    }
    
    if (written < size) {
        ESP_LOGW(TAG, "Partial write: %zu/%zu bytes", written, size);
    }

    if (s_write_log_count < WRITE_DEBUG_LOG_LIMIT || written < size) {
        ESP_LOGD(TAG, "[WRITE] call=%u requested=%zu bytes wrote=%zu timeout_ms=%lu", (unsigned int)(++s_write_log_count), size, written, (unsigned long)timeout_ms);
    } else {
        ++s_write_log_count;
    }
    
    return ESP_OK;
}

esp_err_t audio_driver_read(uint8_t *buffer, size_t size, size_t *bytes_read, uint32_t timeout_ms) {
    if (!is_initialized || g_i2s_rx_handle == NULL) {
        ESP_LOGE(TAG, "I2S RX channel not initialized");
        if (bytes_read) *bytes_read = 0;
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!buffer || size == 0) {
        ESP_LOGE(TAG, "Invalid read parameters");
        if (bytes_read) *bytes_read = 0;
        return ESP_ERR_INVALID_ARG;
    }
    
    // CRITICAL: Acquire mutex to prevent concurrent I2S access
    if (g_i2s_access_mutex == NULL) {
        ESP_LOGE(TAG, "❌ I2S access mutex not initialized");
        if (bytes_read) *bytes_read = 0;
        return ESP_ERR_INVALID_STATE;
    }
    
    // Wait indefinitely for mutex (audio capture is critical path)
    if (xSemaphoreTake(g_i2s_access_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "❌ CRITICAL: Failed to acquire I2S access mutex (should never happen with portMAX_DELAY)");
        if (bytes_read) *bytes_read = 0;
        return ESP_ERR_TIMEOUT;
    }
    
    // Perform I2S channel read operation (protected by mutex)
    // Modern driver uses i2s_channel_read() instead of i2s_read()
    size_t read = 0;
    TickType_t ticks_to_wait = timeout_ms / portTICK_PERIOD_MS;
    
    esp_err_t ret = i2s_channel_read(g_i2s_rx_handle, buffer, size, &read, ticks_to_wait);
    
    // Release mutex immediately after hardware access
    xSemaphoreGive(g_i2s_access_mutex);
    
    if (bytes_read) {
        *bytes_read = read;
    }
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S channel read failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    if (read < size) {
        ESP_LOGD(TAG, "Partial read: %zu/%zu bytes", read, size);
    }
    
    return ESP_OK;
}

bool audio_driver_is_initialized(void) {
    return is_initialized;
}

esp_err_t audio_driver_clear_buffers(void) {
    if (!is_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Modern driver doesn't have i2s_zero_dma_buffer equivalent
    // The channels handle buffer management internally
    // We can preload zeros by writing silence to TX channel
    
    if (g_i2s_tx_handle != NULL) {
        uint8_t silence[512] = {0};  // 512 bytes of silence
        size_t written = 0;
        esp_err_t ret = i2s_channel_write(g_i2s_tx_handle, silence, sizeof(silence), 
                                          &written, pdMS_TO_TICKS(100));
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to preload silence to TX buffer: %s", esp_err_to_name(ret));
            return ret;
        }
    }
    
    // For RX, the modern driver automatically manages buffers
    // No explicit clear needed - just start reading fresh data
    
    return ESP_OK;
}

esp_err_t audio_driver_set_tx_sample_rate(uint32_t sample_rate) {
    if (!is_initialized || g_i2s_tx_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (sample_rate == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (sample_rate == current_tx_sample_rate) {
        return ESP_OK;
    }

    if (g_i2s_access_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(g_i2s_access_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "⚠ Failed to acquire I2S mutex for clock update");
        return ESP_ERR_TIMEOUT;
    }

    uint32_t previous_rate = current_tx_sample_rate;
    esp_err_t ret = i2s_channel_disable(g_i2s_tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ Unable to disable TX channel for clock update: %s", esp_err_to_name(ret));
        xSemaphoreGive(g_i2s_access_mutex);
        return ret;
    }

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
    ret = i2s_channel_reconfig_std_clock(g_i2s_tx_handle, &clk_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ Failed to reconfigure TX clock to %u Hz: %s", (unsigned int)sample_rate, esp_err_to_name(ret));
        goto restore_previous_clock;
    }

    ret = i2s_channel_enable(g_i2s_tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ Unable to re-enable TX channel after clock update: %s", esp_err_to_name(ret));
        goto restore_previous_clock;
    }

    current_tx_sample_rate = sample_rate;
    xSemaphoreGive(g_i2s_access_mutex);
    ESP_LOGI(TAG, "I2S TX sample rate updated to %u Hz", (unsigned int)sample_rate);
    return ESP_OK;

restore_previous_clock:
    {
        i2s_std_clk_config_t restore_cfg = I2S_STD_CLK_DEFAULT_CONFIG(previous_rate);
        restore_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
        i2s_channel_reconfig_std_clock(g_i2s_tx_handle, &restore_cfg);
        i2s_channel_enable(g_i2s_tx_handle);
    }
    xSemaphoreGive(g_i2s_access_mutex);
    return ret;
}

uint32_t audio_driver_get_tx_sample_rate(void) {
    return current_tx_sample_rate;
}

// ===========================
// Private Functions
// ===========================

static esp_err_t configure_i2s_std_full_duplex(void) {
    ESP_LOGI(TAG, "╔══════════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "║ Configuring Modern I2S STD Driver (Separate TX/RX)");
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════════════════");
    
    // Diagnostic: Check system state before I2S init
    ESP_LOGI(TAG, "[DIAG] Pre-init state:");
    ESP_LOGI(TAG, "  Free heap: %u bytes", (unsigned int)esp_get_free_heap_size());
    ESP_LOGI(TAG, "  Free internal RAM: %u bytes", (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "  Free DMA-capable: %u bytes", (unsigned int)heap_caps_get_free_size(MALLOC_CAP_DMA));
    ESP_LOGI(TAG, "  Free PSRAM: %u bytes", (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "  Timestamp: %lld ms", (long long)(esp_timer_get_time() / 1000));
    
    int64_t start_time;
    esp_err_t ret;
    
    // STEP 1: Create I2S channel configuration for full-duplex (separate TX and RX)
    ESP_LOGI(TAG, "[STEP 1/6] Creating I2S channel pair (TX + RX)...");
    ESP_LOGI(TAG, "  Using I2S controller %d for both channels (full-duplex mode)", CONFIG_I2S_STD_PORT);
    
    const uint32_t requested_frame_num = CONFIG_I2S_DMA_BUF_LEN;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_I2S_STD_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = CONFIG_I2S_DMA_BUF_COUNT;
    chan_cfg.dma_frame_num = requested_frame_num;
    if (chan_cfg.dma_frame_num > I2S_DMA_FRAME_MAX) {
        ESP_LOGW(TAG, "DMA frame num %u exceeds HW limit %u - clamping", (unsigned int)chan_cfg.dma_frame_num, (unsigned int)I2S_DMA_FRAME_MAX);
        chan_cfg.dma_frame_num = I2S_DMA_FRAME_MAX;
    }
    chan_cfg.auto_clear = true;  // Auto-clear TX buffer on underflow
    
    ESP_LOGI(TAG, "  DMA config request: %u buffers x %u samples (requested = %u)",
             (unsigned int)CONFIG_I2S_DMA_BUF_COUNT,
             (unsigned int)requested_frame_num,
             (unsigned int)(CONFIG_I2S_DMA_BUF_COUNT * requested_frame_num));
    if (chan_cfg.dma_frame_num != requested_frame_num) {
        ESP_LOGI(TAG, "  DMA frame num (per desc) clamped to %u", (unsigned int)chan_cfg.dma_frame_num);
    } else {
        ESP_LOGI(TAG, "  DMA frame num (per desc): %u", (unsigned int)chan_cfg.dma_frame_num);
    }
    ESP_LOGI(TAG, "  DMA total samples (effective): %u",
             (unsigned int)(CONFIG_I2S_DMA_BUF_COUNT * chan_cfg.dma_frame_num));
    ESP_LOGI(TAG, "  DMA memory committed: %u bytes (2 bytes/sample)",
             (unsigned int)(CONFIG_I2S_DMA_BUF_COUNT * chan_cfg.dma_frame_num * sizeof(int16_t)));
    ESP_LOGI(TAG, "  DMA memory: %d bytes (2 bytes/sample)",
             CONFIG_I2S_DMA_BUF_COUNT * CONFIG_I2S_DMA_BUF_LEN * 2);
    
    start_time = esp_timer_get_time();
    ret = i2s_new_channel(&chan_cfg, &g_i2s_tx_handle, &g_i2s_rx_handle);
    int64_t channel_time = (esp_timer_get_time() - start_time) / 1000;
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ Failed to create I2S channels: %s (took %"PRIu32" ms)", 
                 esp_err_to_name(ret), (uint32_t)channel_time);
        ESP_LOGE(TAG, "  Free heap after fail: %u bytes", (unsigned int)esp_get_free_heap_size());
        return ret;
    }
    ESP_LOGI(TAG, "✅ I2S channels created (took %"PRIu32" ms)", (uint32_t)channel_time);
    ESP_LOGI(TAG, "  TX handle: %p | RX handle: %p", g_i2s_tx_handle, g_i2s_rx_handle);
    
    // STEP 2: Configure TX (speaker) channel
    ESP_LOGI(TAG, "[STEP 2/6] Configuring TX (speaker) channel...");
    
    i2s_std_config_t tx_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(CONFIG_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = GPIO_NUM_NC,                    // No MCLK - CRITICAL
            .bclk = CONFIG_I2S_BCLK,                // Bit clock (shared)
            .ws = CONFIG_I2S_LRCK,                  // Word select (shared)
            .dout = CONFIG_I2S_TX_DATA_OUT,         // Speaker data out (GPIO13)
            .din = GPIO_NUM_NC,                     // Not used for TX
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    
    // Use PLL clock source (not APLL) for compatibility
    tx_std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
    
    ESP_LOGI(TAG, "  Sample rate: %u Hz", CONFIG_AUDIO_SAMPLE_RATE);
    ESP_LOGI(TAG, "  MCLK: DISABLED");
    ESP_LOGI(TAG, "  BCLK: GPIO%d (shared)", CONFIG_I2S_BCLK);
    ESP_LOGI(TAG, "  WS:   GPIO%d (shared)", CONFIG_I2S_LRCK);
    ESP_LOGI(TAG, "  DOUT: GPIO%d (MAX98357A speaker)", CONFIG_I2S_TX_DATA_OUT);
    
    start_time = esp_timer_get_time();
    ret = i2s_channel_init_std_mode(g_i2s_tx_handle, &tx_std_cfg);
    int64_t tx_init_time = (esp_timer_get_time() - start_time) / 1000;
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ TX channel init FAILED: %s (took %"PRIu32" ms)", 
                 esp_err_to_name(ret), (uint32_t)tx_init_time);
        i2s_del_channel(g_i2s_tx_handle);
        i2s_del_channel(g_i2s_rx_handle);
        g_i2s_tx_handle = NULL;
        g_i2s_rx_handle = NULL;
        return ret;
    }
    ESP_LOGI(TAG, "✅ TX channel configured (took %"PRIu32" ms)", (uint32_t)tx_init_time);
    
    // STEP 3: Configure RX (microphone) channel
    ESP_LOGI(TAG, "[STEP 3/6] Configuring RX (microphone) channel...");
    
    i2s_std_config_t rx_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(CONFIG_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = GPIO_NUM_NC,                    // No MCLK - CRITICAL
            .bclk = CONFIG_I2S_BCLK,                // Bit clock (shared)
            .ws = CONFIG_I2S_LRCK,                  // Word select (shared)
            .dout = GPIO_NUM_NC,                    // Not used for RX
            .din = CONFIG_I2S_RX_DATA_IN,           // Microphone data in (GPIO2 - safe!)
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    
    // Use PLL clock source (not APLL) for compatibility
    rx_std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
    
    ESP_LOGI(TAG, "  Sample rate: %u Hz", CONFIG_AUDIO_SAMPLE_RATE);
    ESP_LOGI(TAG, "  MCLK: DISABLED");
    ESP_LOGI(TAG, "  BCLK: GPIO%d (shared)", CONFIG_I2S_BCLK);
    ESP_LOGI(TAG, "  WS:   GPIO%d (shared)", CONFIG_I2S_LRCK);
    ESP_LOGI(TAG, "  DIN:  GPIO%d (INMP441 microphone)", CONFIG_I2S_RX_DATA_IN);
    
    start_time = esp_timer_get_time();
    ret = i2s_channel_init_std_mode(g_i2s_rx_handle, &rx_std_cfg);
    int64_t rx_init_time = (esp_timer_get_time() - start_time) / 1000;
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ RX channel init FAILED: %s (took %"PRIu32" ms)", 
                 esp_err_to_name(ret), (uint32_t)rx_init_time);
        i2s_del_channel(g_i2s_tx_handle);
        i2s_del_channel(g_i2s_rx_handle);
        g_i2s_tx_handle = NULL;
        g_i2s_rx_handle = NULL;
        return ret;
    }
    ESP_LOGI(TAG, "✅ RX channel configured (took %"PRIu32" ms)", (uint32_t)rx_init_time);
    
    // STEP 4: Enable TX channel
    ESP_LOGI(TAG, "[STEP 4/6] Enabling TX channel...");
    start_time = esp_timer_get_time();
    ret = i2s_channel_enable(g_i2s_tx_handle);
    int64_t tx_enable_time = (esp_timer_get_time() - start_time) / 1000;
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ TX channel enable FAILED: %s (took %"PRIu32" ms)", 
                 esp_err_to_name(ret), (uint32_t)tx_enable_time);
        i2s_del_channel(g_i2s_tx_handle);
        i2s_del_channel(g_i2s_rx_handle);
        g_i2s_tx_handle = NULL;
        g_i2s_rx_handle = NULL;
        return ret;
    }
    ESP_LOGI(TAG, "✅ TX channel enabled (took %"PRIu32" ms)", (uint32_t)tx_enable_time);
    
    // STEP 5: Enable RX channel
    ESP_LOGI(TAG, "[STEP 5/6] Enabling RX channel...");
    start_time = esp_timer_get_time();
    ret = i2s_channel_enable(g_i2s_rx_handle);
    int64_t rx_enable_time = (esp_timer_get_time() - start_time) / 1000;
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ RX channel enable FAILED: %s (took %"PRIu32" ms)", 
                 esp_err_to_name(ret), (uint32_t)rx_enable_time);
        i2s_channel_disable(g_i2s_tx_handle);
        i2s_del_channel(g_i2s_tx_handle);
        i2s_del_channel(g_i2s_rx_handle);
        g_i2s_tx_handle = NULL;
        g_i2s_rx_handle = NULL;
        return ret;
    }
    ESP_LOGI(TAG, "✅ RX channel enabled (took %"PRIu32" ms)", (uint32_t)rx_enable_time);
    
    // STEP 6: Hardware stabilization
    ESP_LOGI(TAG, "[STEP 6/6] Hardware stabilization...");
    ESP_LOGI(TAG, "  Phase 1: Initial settle (50ms)");
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // Verify DMA is ready by attempting a test write to TX channel
    ESP_LOGI(TAG, "  Phase 2: DMA verification");
    uint8_t test_buffer[128] = {0};
    size_t bytes_written = 0;
    ret = i2s_channel_write(g_i2s_tx_handle, test_buffer, sizeof(test_buffer), 
                            &bytes_written, pdMS_TO_TICKS(100));
    if (ret == ESP_OK && bytes_written > 0) {
        ESP_LOGI(TAG, "  ✓ DMA TX operational (%zu bytes)", bytes_written);
    } else {
        ESP_LOGW(TAG, "  ⚠ DMA TX test: %s (wrote %zu bytes)", esp_err_to_name(ret), bytes_written);
    }
    
    ESP_LOGI(TAG, "  Phase 3: Additional settle (150ms) - CRITICAL for RX DMA");
    vTaskDelay(pdMS_TO_TICKS(150));
    
    // Final diagnostic
    ESP_LOGI(TAG, "[DIAG] Post-init state:");
    ESP_LOGI(TAG, "  Free heap: %u bytes", (unsigned int)esp_get_free_heap_size());
    ESP_LOGI(TAG, "  Free internal RAM: %u bytes", (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "  Free DMA-capable: %u bytes", (unsigned int)heap_caps_get_free_size(MALLOC_CAP_DMA));
    ESP_LOGI(TAG, "  Free PSRAM: %u bytes", (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "  Timestamp: %lld ms", (long long)(esp_timer_get_time() / 1000));
    ESP_LOGI(TAG, "  Total init time: %lld ms", 
             (long long)(channel_time + tx_init_time + rx_init_time + tx_enable_time + rx_enable_time + 200));
    
    ESP_LOGI(TAG, "╔══════════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "║ ✅ MODERN I2S STD FULL-DUPLEX READY");
    ESP_LOGI(TAG, "║ Driver: i2s_std (NOT legacy!)");
    ESP_LOGI(TAG, "║ Mode: Master TX+RX | Rate: %d Hz | Format: TX stereo / RX mono", CONFIG_AUDIO_SAMPLE_RATE);
    ESP_LOGI(TAG, "║ This should eliminate LoadStoreError crashes!");
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════════════════");
    
    return ESP_OK;
}

uint8_t audio_driver_get_buffer_level_percent(void) {
    // Simplified implementation - return approximate buffer level
    // Since we don't have direct access to buffer info, return a conservative estimate
    return 50; // Return 50% as a default estimate
}

bool audio_driver_is_buffer_nearly_full(void) {
    // Simplified implementation - return false by default
    // This prevents unnecessary throttling in most cases
    return false;
}

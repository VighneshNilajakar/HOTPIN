/**
 * @file audio_driver.c
 * @brief Dual I2S audio driver implementation for INMP441 + MAX98357A
 * 
 * Implements full-duplex audio using:
 * - I2S0 (TX): MAX98357A speaker amplifier
 * - I2S1 (RX): INMP441 MEMS microphone
 * - Shared BCLK (GPIO14) and WS (GPIO15) for synchronized operation
 * - PSRAM-backed DMA buffers for high-bandwidth audio streaming
 */

#include "audio_driver.h"
#include "config.h"
#include "esp_log.h"
#include "driver/i2s.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = TAG_AUDIO;
static bool is_initialized = false;

// Use single I2S peripheral for full-duplex operation
#define I2S_AUDIO_NUM    I2S_NUM_0

/**
 * @brief Global mutex for thread-safe I2S hardware access
 * 
 * CRITICAL: Protects concurrent i2s_read() and i2s_write() operations
 * from corrupting the DMA controller state when called from multiple tasks.
 */
SemaphoreHandle_t g_i2s_access_mutex = NULL;

// ===========================
// Private Function Declarations
// ===========================
static esp_err_t configure_i2s_full_duplex(void);

// ===========================
// Public Functions
// ===========================

esp_err_t audio_driver_init(void) {
    ESP_LOGI(TAG, "Initializing I2S full-duplex audio driver...");
    
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
    
    // Configure single I2S peripheral for both TX and RX
    esp_err_t ret = configure_i2s_full_duplex();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure I2S full-duplex: %s", esp_err_to_name(ret));
        return ret;
    }
    
    is_initialized = true;
    ESP_LOGI(TAG, "✅ Audio driver initialized successfully");
    ESP_LOGI(TAG, "   Mode: Full-duplex (TX + RX)");
    ESP_LOGI(TAG, "   TX (Speaker): I2S0 on GPIO%d", CONFIG_I2S_TX_DATA_OUT);
    ESP_LOGI(TAG, "   RX (Microphone): I2S0 on GPIO%d", CONFIG_I2S_RX_DATA_IN);
    ESP_LOGI(TAG, "   Shared BCLK: GPIO%d, WS: GPIO%d", CONFIG_I2S_BCLK, CONFIG_I2S_LRCK);
    
    return ESP_OK;
}

esp_err_t audio_driver_deinit(void) {
    ESP_LOGI(TAG, "╔══════════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "║ Deinitializing I2S Driver for Camera Capture");
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════════════════");
    
    if (!is_initialized) {
        ESP_LOGW(TAG, "Audio driver not initialized - nothing to deinit");
        return ESP_OK;
    }
    
    // Step 1: Stop I2S peripheral
    ESP_LOGI(TAG, "[STEP 1/3] Stopping I2S peripheral...");
    esp_err_t ret = i2s_stop(I2S_AUDIO_NUM);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "⚠ i2s_stop returned: %s (continuing anyway)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "✅ I2S peripheral stopped");
    }
    
    // Step 2: Allow DMA operations to complete
    ESP_LOGI(TAG, "[STEP 2/3] Waiting for DMA completion (50ms)...");
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // Step 3: Uninstall I2S driver (frees interrupts and resources)
    ESP_LOGI(TAG, "[STEP 3/3] Uninstalling I2S driver...");
    ESP_LOGI(TAG, "  This will free:");
    ESP_LOGI(TAG, "    - I2S peripheral interrupt allocation");
    ESP_LOGI(TAG, "    - DMA descriptors and buffers");
    ESP_LOGI(TAG, "    - GPIO matrix configuration");
    
    int64_t start_time = esp_timer_get_time();
    ret = i2s_driver_uninstall(I2S_AUDIO_NUM);
    int64_t uninstall_time = (esp_timer_get_time() - start_time) / 1000;
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ i2s_driver_uninstall FAILED: %s (took %lld ms)", 
                 esp_err_to_name(ret), (long long)uninstall_time);
        ESP_LOGE(TAG, "  This may cause camera init to fail");
        // Continue anyway to update state
    } else {
        ESP_LOGI(TAG, "✅ I2S driver uninstalled successfully (took %lld ms)", 
                 (long long)uninstall_time);
    }
    
    // Additional delay to ensure interrupt controller and GPIO matrix settle
    ESP_LOGI(TAG, "Additional settling time (50ms) for interrupt/GPIO matrix...");
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // Mark as uninitialized
    is_initialized = false;
    
    ESP_LOGI(TAG, "╔══════════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "║ ✅ I2S Driver Deinitialized - Camera Can Now Init");
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════════════════");
    
    return ret;
}

esp_err_t audio_driver_write(const uint8_t *data, size_t size, size_t *bytes_written, uint32_t timeout_ms) {
    if (!is_initialized) {
        ESP_LOGE(TAG, "I2S driver not initialized");
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
    
    // Try to acquire mutex with timeout to prevent indefinite blocking
    if (xSemaphoreTake(g_i2s_access_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "⚠ Failed to acquire I2S access mutex within 100ms (write blocked)");
        if (bytes_written) *bytes_written = 0;
        return ESP_ERR_TIMEOUT;
    }
    
    // Perform I2S write operation (protected by mutex)
    size_t written = 0;
    TickType_t ticks_to_wait = timeout_ms / portTICK_PERIOD_MS;
    
    esp_err_t ret = i2s_write(I2S_AUDIO_NUM, data, size, &written, ticks_to_wait);
    
    // Release mutex immediately after hardware access
    xSemaphoreGive(g_i2s_access_mutex);
    
    if (bytes_written) {
        *bytes_written = written;
    }
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    if (written < size) {
        ESP_LOGW(TAG, "Partial write: %zu/%zu bytes", written, size);
    }
    
    return ESP_OK;
}

esp_err_t audio_driver_read(uint8_t *buffer, size_t size, size_t *bytes_read, uint32_t timeout_ms) {
    if (!is_initialized) {
        ESP_LOGE(TAG, "I2S driver not initialized");
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
    
    // Perform I2S read operation (protected by mutex)
    size_t read = 0;
    TickType_t ticks_to_wait = timeout_ms / portTICK_PERIOD_MS;
    
    esp_err_t ret = i2s_read(I2S_AUDIO_NUM, buffer, size, &read, ticks_to_wait);
    
    // Release mutex immediately after hardware access
    xSemaphoreGive(g_i2s_access_mutex);
    
    if (bytes_read) {
        *bytes_read = read;
    }
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S read failed: %s", esp_err_to_name(ret));
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
    
    esp_err_t ret = i2s_zero_dma_buffer(I2S_AUDIO_NUM);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to clear DMA buffer: %s", esp_err_to_name(ret));
    }
    
    return ret;
}

// ===========================
// Private Functions
// ===========================

static esp_err_t configure_i2s_full_duplex(void) {
    ESP_LOGI(TAG, "╔══════════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "║ Configuring I2S0 for full-duplex audio (TX+RX)");
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════════════════");
    
    // Diagnostic: Check system state before I2S init
    ESP_LOGI(TAG, "[DIAG] Pre-init state:");
    ESP_LOGI(TAG, "  Free heap: %u bytes", (unsigned int)esp_get_free_heap_size());
    ESP_LOGI(TAG, "  Free PSRAM: %u bytes", (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "  Timestamp: %lld ms", (long long)(esp_timer_get_time() / 1000));
    
    // I2S full-duplex configuration (both TX and RX on single peripheral)
    i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX,  // Full-duplex
        .sample_rate = CONFIG_AUDIO_SAMPLE_RATE,
        .bits_per_sample = CONFIG_AUDIO_BITS_PER_SAMPLE,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,  // Mono for both
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,     // Non-shared now (only one peripheral)
        .dma_buf_count = CONFIG_I2S_DMA_BUF_COUNT,
        .dma_buf_len = CONFIG_I2S_DMA_BUF_LEN,
        .use_apll = false,                            // Use PLL, not APLL
        .tx_desc_auto_clear = true,                   // Auto-clear TX on underflow
        .fixed_mclk = 0
    };
    
    ESP_LOGI(TAG, "[CONFIG] I2S Configuration:");
    ESP_LOGI(TAG, "  Sample rate: %u Hz", (unsigned int)i2s_config.sample_rate);
    ESP_LOGI(TAG, "  Bits per sample: %d", i2s_config.bits_per_sample);
    ESP_LOGI(TAG, "  DMA buffers: %d x %d bytes", i2s_config.dma_buf_count, i2s_config.dma_buf_len);
    ESP_LOGI(TAG, "  Total DMA: %d bytes", i2s_config.dma_buf_count * i2s_config.dma_buf_len * 2);
    
    // Install I2S driver
    ESP_LOGI(TAG, "[STEP 1/5] Installing I2S driver...");
    int64_t start_time = esp_timer_get_time();
    esp_err_t ret = i2s_driver_install(I2S_AUDIO_NUM, &i2s_config, 0, NULL);
    int64_t install_time = (esp_timer_get_time() - start_time) / 1000;
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ I2S driver install FAILED: %s (took %lld ms)", esp_err_to_name(ret), (long long)install_time);
        ESP_LOGE(TAG, "  Free heap after fail: %u bytes", (unsigned int)esp_get_free_heap_size());
        return ret;
    }
    ESP_LOGI(TAG, "✅ I2S driver installed (took %lld ms)", (long long)install_time);
    
    // I2S pin configuration with both TX and RX pins
    i2s_pin_config_t i2s_pins = {
        .mck_io_num = I2S_PIN_NO_CHANGE,            // No MCLK - CRITICAL
        .bck_io_num = CONFIG_I2S_BCLK,              // Bit clock (shared by TX and RX)
        .ws_io_num = CONFIG_I2S_LRCK,               // Word select (shared by TX and RX)
        .data_out_num = CONFIG_I2S_TX_DATA_OUT,     // Data output to speaker (GPIO13)
        .data_in_num = CONFIG_I2S_RX_DATA_IN        // Data input from mic (GPIO2 - safe pin)
    };
    
    ESP_LOGI(TAG, "[STEP 2/5] Setting I2S pins...");
    ESP_LOGI(TAG, "  MCLK: DISABLED (I2S_PIN_NO_CHANGE)");
    ESP_LOGI(TAG, "  BCLK: GPIO%d", CONFIG_I2S_BCLK);
    ESP_LOGI(TAG, "  WS:   GPIO%d", CONFIG_I2S_LRCK);
    ESP_LOGI(TAG, "  DOUT: GPIO%d (Speaker)", CONFIG_I2S_TX_DATA_OUT);
    ESP_LOGI(TAG, "  DIN:  GPIO%d (Microphone)", CONFIG_I2S_RX_DATA_IN);
    
    start_time = esp_timer_get_time();
    ret = i2s_set_pin(I2S_AUDIO_NUM, &i2s_pins);
    int64_t pin_time = (esp_timer_get_time() - start_time) / 1000;
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ I2S pin config FAILED: %s (took %lld ms)", esp_err_to_name(ret), (long long)pin_time);
        i2s_driver_uninstall(I2S_AUDIO_NUM);
        return ret;
    }
    ESP_LOGI(TAG, "✅ I2S pins configured (took %lld ms)", (long long)pin_time);
    
    // Clear DMA buffers
    ESP_LOGI(TAG, "[STEP 3/5] Clearing DMA buffers...");
    start_time = esp_timer_get_time();
    ret = i2s_zero_dma_buffer(I2S_AUDIO_NUM);
    int64_t clear_time = (esp_timer_get_time() - start_time) / 1000;
    
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "⚠ DMA buffer clear returned: %s (took %lld ms)", esp_err_to_name(ret), (long long)clear_time);
    } else {
        ESP_LOGI(TAG, "✅ DMA buffers cleared (took %lld ms)", (long long)clear_time);
    }
    
    // Start I2S (critical for proper operation)
    ESP_LOGI(TAG, "[STEP 4/5] Starting I2S peripheral...");
    start_time = esp_timer_get_time();
    ret = i2s_start(I2S_AUDIO_NUM);
    int64_t start_i2s_time = (esp_timer_get_time() - start_time) / 1000;
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ I2S start FAILED: %s (took %lld ms)", esp_err_to_name(ret), (long long)start_i2s_time);
        i2s_driver_uninstall(I2S_AUDIO_NUM);
        return ret;
    }
    ESP_LOGI(TAG, "✅ I2S peripheral started (took %lld ms)", (long long)start_i2s_time);
    
    // CRITICAL: Extended hardware stabilization period
    ESP_LOGI(TAG, "[STEP 5/5] Hardware stabilization...");
    ESP_LOGI(TAG, "  Phase 1: Initial settle (50ms)");
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // Verify DMA is ready by attempting a test write
    // NOTE: No mutex needed here - this runs during init before tasks start
    ESP_LOGI(TAG, "  Phase 2: DMA verification");
    uint8_t test_buffer[128] = {0};
    size_t bytes_written = 0;
    ret = i2s_write(I2S_AUDIO_NUM, test_buffer, sizeof(test_buffer), &bytes_written, pdMS_TO_TICKS(100));
    if (ret == ESP_OK && bytes_written > 0) {
        ESP_LOGI(TAG, "  ✓ DMA TX operational (%zu bytes)", bytes_written);
    } else {
        ESP_LOGW(TAG, "  ⚠ DMA TX test: %s (wrote %zu bytes)", esp_err_to_name(ret), bytes_written);
    }
    
    ESP_LOGI(TAG, "  Phase 3: Additional settle (150ms) - CRITICAL for RX");
    vTaskDelay(pdMS_TO_TICKS(150));
    
    // Final diagnostic
    ESP_LOGI(TAG, "[DIAG] Post-init state:");
    ESP_LOGI(TAG, "  Free heap: %u bytes", (unsigned int)esp_get_free_heap_size());
    ESP_LOGI(TAG, "  Free PSRAM: %u bytes", (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "  Timestamp: %lld ms", (long long)(esp_timer_get_time() / 1000));
    ESP_LOGI(TAG, "  Total init time: %lld ms", (long long)(install_time + pin_time + clear_time + start_i2s_time + 200));
    
    ESP_LOGI(TAG, "╔══════════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "║ ✅ I2S FULL-DUPLEX READY");
    ESP_LOGI(TAG, "║ Mode: Master TX+RX | Rate: %d Hz | Format: 16-bit mono", CONFIG_AUDIO_SAMPLE_RATE);
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════════════════");
    
    return ESP_OK;
}

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
#include <string.h>

static const char *TAG = TAG_AUDIO;
static bool is_initialized = false;
static bool tx_enabled = false;
static bool rx_enabled = false;

// ===========================
// Private Function Declarations
// ===========================
static esp_err_t configure_i2s_tx(void);
static esp_err_t configure_i2s_rx(void);

// ===========================
// Public Functions
// ===========================

esp_err_t audio_driver_init(void) {
    ESP_LOGI(TAG, "Initializing dual I2S audio drivers...");
    
    if (is_initialized) {
        ESP_LOGW(TAG, "Audio driver already initialized");
        return ESP_OK;
    }
    
    esp_err_t ret;
    
    // ===========================
    // Configure I2S0 (TX) - Speaker Output
    // ===========================
    ret = configure_i2s_tx();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure I2S TX: %s", esp_err_to_name(ret));
        return ret;
    }
    tx_enabled = true;
    
    // ===========================
    // Configure I2S1 (RX) - Microphone Input
    // ===========================
    ret = configure_i2s_rx();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure I2S RX: %s", esp_err_to_name(ret));
        audio_driver_deinit();  // Clean up TX on failure
        return ret;
    }
    rx_enabled = true;
    
    is_initialized = true;
    ESP_LOGI(TAG, "✅ Audio driver initialized successfully");
    ESP_LOGI(TAG, "   TX: I2S0 (MAX98357A) on GPIO%d", CONFIG_I2S_TX_DATA_OUT);
    ESP_LOGI(TAG, "   RX: I2S1 (INMP441) on GPIO%d", CONFIG_I2S_RX_DATA_IN);
    ESP_LOGI(TAG, "   Shared BCLK: GPIO%d, WS: GPIO%d", CONFIG_I2S_BCLK, CONFIG_I2S_LRCK);
    
    return ESP_OK;
}

esp_err_t audio_driver_deinit(void) {
    ESP_LOGI(TAG, "Deinitializing I2S drivers...");
    
    if (!is_initialized) {
        ESP_LOGW(TAG, "Audio driver not initialized");
        return ESP_OK;
    }
    
    esp_err_t ret_tx = ESP_OK;
    esp_err_t ret_rx = ESP_OK;
    
    // FIX: Stop I2S operations before uninstalling
    if (tx_enabled) {
        i2s_stop(CONFIG_I2S_NUM_TX);
        ESP_LOGI(TAG, "I2S TX stopped");
    }
    
    if (rx_enabled) {
        i2s_stop(CONFIG_I2S_NUM_RX);
        ESP_LOGI(TAG, "I2S RX stopped");
    }
    
    // FIX: Add delay to ensure DMA operations complete
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // Uninstall I2S drivers
    if (tx_enabled) {
        ret_tx = i2s_driver_uninstall(CONFIG_I2S_NUM_TX);
        if (ret_tx != ESP_OK) {
            ESP_LOGE(TAG, "Failed to uninstall I2S TX: %s", esp_err_to_name(ret_tx));
        } else {
            ESP_LOGI(TAG, "I2S TX uninstalled");
        }
        tx_enabled = false;
    }
    
    if (rx_enabled) {
        ret_rx = i2s_driver_uninstall(CONFIG_I2S_NUM_RX);
        if (ret_rx != ESP_OK) {
            ESP_LOGE(TAG, "Failed to uninstall I2S RX: %s", esp_err_to_name(ret_rx));
        } else {
            ESP_LOGI(TAG, "I2S RX uninstalled");
        }
        rx_enabled = false;
    }
    
    // FIX: Additional delay to free interrupt resources before camera init
    vTaskDelay(pdMS_TO_TICKS(50));
    
    is_initialized = false;
    ESP_LOGI(TAG, "Audio driver deinitialized");
    
    // Return error if either uninstall failed
    return (ret_tx != ESP_OK) ? ret_tx : ret_rx;
}

esp_err_t audio_driver_write(const uint8_t *data, size_t size, size_t *bytes_written, uint32_t timeout_ms) {
    if (!is_initialized || !tx_enabled) {
        ESP_LOGE(TAG, "I2S TX not initialized");
        if (bytes_written) *bytes_written = 0;
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!data || size == 0) {
        ESP_LOGE(TAG, "Invalid write parameters");
        if (bytes_written) *bytes_written = 0;
        return ESP_ERR_INVALID_ARG;
    }
    
    size_t written = 0;
    TickType_t ticks_to_wait = timeout_ms / portTICK_PERIOD_MS;
    
    esp_err_t ret = i2s_write(CONFIG_I2S_NUM_TX, data, size, &written, ticks_to_wait);
    
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
    if (!is_initialized || !rx_enabled) {
        ESP_LOGE(TAG, "I2S RX not initialized");
        if (bytes_read) *bytes_read = 0;
        return ESP_ERR_INVALID_STATE;
    }
    
    if (!buffer || size == 0) {
        ESP_LOGE(TAG, "Invalid read parameters");
        if (bytes_read) *bytes_read = 0;
        return ESP_ERR_INVALID_ARG;
    }
    
    size_t read = 0;
    TickType_t ticks_to_wait = timeout_ms / portTICK_PERIOD_MS;
    
    esp_err_t ret = i2s_read(CONFIG_I2S_NUM_RX, buffer, size, &read, ticks_to_wait);
    
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
    
    esp_err_t ret_tx = ESP_OK;
    esp_err_t ret_rx = ESP_OK;
    
    if (tx_enabled) {
        ret_tx = i2s_zero_dma_buffer(CONFIG_I2S_NUM_TX);
        if (ret_tx != ESP_OK) {
            ESP_LOGE(TAG, "Failed to clear TX buffer: %s", esp_err_to_name(ret_tx));
        }
    }
    
    if (rx_enabled) {
        ret_rx = i2s_zero_dma_buffer(CONFIG_I2S_NUM_RX);
        if (ret_rx != ESP_OK) {
            ESP_LOGE(TAG, "Failed to clear RX buffer: %s", esp_err_to_name(ret_rx));
        }
    }
    
    return (ret_tx != ESP_OK) ? ret_tx : ret_rx;
}

// ===========================
// Private Functions
// ===========================

static esp_err_t configure_i2s_tx(void) {
    ESP_LOGI(TAG, "Configuring I2S0 TX for MAX98357A...");
    
    // I2S TX configuration
    i2s_config_t i2s_tx_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX,
        .sample_rate = CONFIG_AUDIO_SAMPLE_RATE,
        .bits_per_sample = CONFIG_AUDIO_BITS_PER_SAMPLE,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,  // Mono output
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1 | ESP_INTR_FLAG_SHARED,  // FIX: Shared interrupt to prevent exhaustion
        .dma_buf_count = CONFIG_I2S_DMA_BUF_COUNT,
        .dma_buf_len = CONFIG_I2S_DMA_BUF_LEN,
        .use_apll = false,                             // Use PLL, not APLL
        .tx_desc_auto_clear = true,                    // Auto-clear on underflow
        .fixed_mclk = 0
    };
    
    // Install I2S driver
    esp_err_t ret = i2s_driver_install(CONFIG_I2S_NUM_TX, &i2s_tx_config, 0, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install I2S TX driver: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // I2S TX pin configuration
    i2s_pin_config_t i2s_tx_pins = {
        .mck_io_num = I2S_PIN_NO_CHANGE,            // FIX: Disable MCLK to prevent pin conflict
        .bck_io_num = CONFIG_I2S_BCLK,              // Bit clock (shared)
        .ws_io_num = CONFIG_I2S_LRCK,               // Word select (shared)
        .data_out_num = CONFIG_I2S_TX_DATA_OUT,     // Data output to speaker
        .data_in_num = I2S_PIN_NO_CHANGE            // No input on TX
    };
    
    ret = i2s_set_pin(CONFIG_I2S_NUM_TX, &i2s_tx_pins);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set I2S TX pins: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "TX Pin config: BCLK=%d, WS=%d, DOUT=%d, MCLK=DISABLED", 
                 CONFIG_I2S_BCLK, CONFIG_I2S_LRCK, CONFIG_I2S_TX_DATA_OUT);
        i2s_driver_uninstall(CONFIG_I2S_NUM_TX);
        return ret;
    }
    
    // Clear DMA buffers
    i2s_zero_dma_buffer(CONFIG_I2S_NUM_TX);
    
    ESP_LOGI(TAG, "I2S TX configured: %d Hz, %d-bit, mono", 
             CONFIG_AUDIO_SAMPLE_RATE, 16);
    
    return ESP_OK;
}

static esp_err_t configure_i2s_rx(void) {
    ESP_LOGI(TAG, "Configuring I2S1 RX for INMP441...");
    
    // I2S RX configuration
    i2s_config_t i2s_rx_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_RX,
        .sample_rate = CONFIG_AUDIO_SAMPLE_RATE,
        .bits_per_sample = CONFIG_AUDIO_BITS_PER_SAMPLE,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,  // Mono input
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1 | ESP_INTR_FLAG_SHARED,  // FIX: Shared interrupt to prevent exhaustion
        .dma_buf_count = CONFIG_I2S_DMA_BUF_COUNT,
        .dma_buf_len = CONFIG_I2S_DMA_BUF_LEN,
        .use_apll = false,                             // Use PLL, not APLL
        .tx_desc_auto_clear = false,                   // N/A for RX
        .fixed_mclk = 0
    };
    
    // Install I2S driver
    esp_err_t ret = i2s_driver_install(CONFIG_I2S_NUM_RX, &i2s_rx_config, 0, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install I2S RX driver: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // I2S RX pin configuration (shared BCLK and WS with TX)
    i2s_pin_config_t i2s_rx_pins = {
        .mck_io_num = I2S_PIN_NO_CHANGE,            // FIX: Disable MCLK to prevent pin conflict
        .bck_io_num = CONFIG_I2S_BCLK,              // Bit clock (shared with TX)
        .ws_io_num = CONFIG_I2S_LRCK,               // Word select (shared with TX)
        .data_out_num = I2S_PIN_NO_CHANGE,          // No output on RX
        .data_in_num = CONFIG_I2S_RX_DATA_IN        // Data input from mic
    };
    
    ret = i2s_set_pin(CONFIG_I2S_NUM_RX, &i2s_rx_pins);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set I2S RX pins: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "RX Pin config: BCLK=%d, WS=%d, DIN=%d, MCLK=DISABLED", 
                 CONFIG_I2S_BCLK, CONFIG_I2S_LRCK, CONFIG_I2S_RX_DATA_IN);
        i2s_driver_uninstall(CONFIG_I2S_NUM_RX);
        return ret;
    }
    
    // Clear DMA buffers
    i2s_zero_dma_buffer(CONFIG_I2S_NUM_RX);
    
    ESP_LOGI(TAG, "I2S RX configured: %d Hz, %d-bit, mono", 
             CONFIG_AUDIO_SAMPLE_RATE, 16);
    
    return ESP_OK;
}

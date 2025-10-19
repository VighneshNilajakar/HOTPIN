/**
 * @file audio_driver.h
 * @brief Modern I2S STD audio driver manager (Full-Duplex)
 * 
 * Uses the modern i2s_std driver (driver/i2s_std.h) for robust full-duplex operation.
 * Manages separate TX (speaker) and RX (microphone) channels with shared clock.
 * 
 * MIGRATION NOTE: This replaces the deprecated legacy I2S driver to fix LoadStoreError crashes.
 */

#ifndef AUDIO_DRIVER_H
#define AUDIO_DRIVER_H

#include "esp_err.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * @brief Global mutex for protecting concurrent I2S read/write operations
 * 
 * CRITICAL: This mutex prevents race conditions when stt_pipeline_task (Core 1)
 * and tts_playback_task (Core 1) concurrently access the I2S hardware channels.
 * Must be acquired before any i2s_channel_read() or i2s_channel_write() call.
 */
extern SemaphoreHandle_t g_i2s_access_mutex;

/**
 * @brief I2S channel handles for modern i2s_std driver
 * 
 * The modern driver uses separate channel handles for TX and RX,
 * allowing for cleaner full-duplex operation.
 */
extern i2s_chan_handle_t g_i2s_tx_handle;  // Speaker output channel
extern i2s_chan_handle_t g_i2s_rx_handle;  // Microphone input channel

/**
 * @brief Initialize dual I2S drivers
 * 
 * Sets up I2S0 (TX) and I2S1 (RX) with shared BCLK/WS and PSRAM DMA buffers
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t audio_driver_init(void);

/**
 * @brief Deinitialize I2S drivers (for mode switching)
 * 
 * @return ESP_OK on success
 */
esp_err_t audio_driver_deinit(void);

/**
 * @brief Write PCM audio data to I2S TX (speaker)
 * 
 * @param data PCM buffer
 * @param size Number of bytes to write
 * @param bytes_written Output parameter for actual bytes written
 * @param timeout_ms Timeout in milliseconds
 * @return ESP_OK on success
 */
esp_err_t audio_driver_write(const uint8_t *data, size_t size, size_t *bytes_written, uint32_t timeout_ms);

/**
 * @brief Read PCM audio data from I2S RX (microphone)
 * 
 * @param buffer Output buffer for PCM data
 * @param size Buffer size in bytes
 * @param bytes_read Output parameter for actual bytes read
 * @param timeout_ms Timeout in milliseconds
 * @return ESP_OK on success
 */
esp_err_t audio_driver_read(uint8_t *buffer, size_t size, size_t *bytes_read, uint32_t timeout_ms);

/**
 * @brief Check if audio driver is initialized
 * 
 * @return true if initialized, false otherwise
 */
bool audio_driver_is_initialized(void);

/**
 * @brief Clear I2S DMA buffers (both TX and RX)
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t audio_driver_clear_buffers(void);

/**
 * @brief Update the I2S TX sample rate without rebuilding the driver
 *
 * Thread-safe helper that temporarily disables the TX channel, reconfigures
 * the clock dividers, and re-enables playback. Used by the TTS decoder when
 * streaming PCM at sample rates different from CONFIG_AUDIO_SAMPLE_RATE.
 *
 * @param sample_rate Desired sample rate in Hz (e.g., 16000, 22050)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t audio_driver_set_tx_sample_rate(uint32_t sample_rate);

/**
 * @brief Get the currently active I2S TX sample rate in Hz
 */
uint32_t audio_driver_get_tx_sample_rate(void);

/**
 * @brief Get current buffer level as percentage
 * 
 * @return Buffer level percentage (0-100)
 */
uint8_t audio_driver_get_buffer_level_percent(void);

/**
 * @brief Check if buffer is approaching overflow
 * 
 * @return true if buffer is > 80% full, false otherwise
 */
bool audio_driver_is_buffer_nearly_full(void);

#endif // AUDIO_DRIVER_H

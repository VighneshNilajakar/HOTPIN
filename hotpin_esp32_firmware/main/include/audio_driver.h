/**
 * @file audio_driver.h
 * @brief Dual I2S audio driver manager
 * 
 * Manages I2S0 (TX/speaker) and I2S1 (RX/microphone) with shared clock
 */

#ifndef AUDIO_DRIVER_H
#define AUDIO_DRIVER_H

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

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

#endif // AUDIO_DRIVER_H

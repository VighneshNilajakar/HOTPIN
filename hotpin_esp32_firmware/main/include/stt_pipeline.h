/**
 * @file stt_pipeline.h
 * @brief Speech-to-text audio pipeline
 * 
 * Manages audio capture from I2S RX and streaming to WebSocket
 */

#ifndef STT_PIPELINE_H
#define STT_PIPELINE_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Initialize STT pipeline
 * 
 * Creates ring buffer and starts audio capture task
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t stt_pipeline_init(void);

/**
 * @brief Deinitialize STT pipeline
 * 
 * @return ESP_OK on success
 */
esp_err_t stt_pipeline_deinit(void);

/**
 * @brief Start STT pipeline with audio capture and streaming
 * 
 * Creates capture and streaming tasks, starts audio recording
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t stt_pipeline_start(void);

/**
 * @brief Stop STT pipeline
 * 
 * Stops capture, sends EOS signal, and destroys tasks
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t stt_pipeline_stop(void);

/**
 * @brief Check if pipeline is recording
 * 
 * @return true if recording, false otherwise
 */
bool stt_pipeline_is_recording(void);

/**
 * @brief Cancel any pending capture and preserve current ring buffer state
 */
void stt_pipeline_cancel_capture(void);

#endif // STT_PIPELINE_H

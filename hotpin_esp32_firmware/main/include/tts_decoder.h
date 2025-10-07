/**
 * @file tts_decoder.h
 * @brief Text-to-speech audio decoder
 * 
 * Handles WAV audio reception from WebSocket and playback via I2S TX
 */

#ifndef TTS_DECODER_H
#define TTS_DECODER_H

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * @brief Initialize TTS decoder
 * 
 * Creates decoder task and buffers
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t tts_decoder_init(void);

/**
 * @brief Deinitialize TTS decoder
 * 
 * @return ESP_OK on success
 */
esp_err_t tts_decoder_deinit(void);

/**
 * @brief Start TTS decoder and playback task
 * 
 * Creates playback task and begins processing audio chunks
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t tts_decoder_start(void);

/**
 * @brief Stop TTS decoder and playback
 * 
 * Stops playback task and cleans up resources
 * 
 * @return ESP_OK on success
 */
esp_err_t tts_decoder_stop(void);

/**
 * @brief Check if TTS is currently playing
 * 
 * @return true if playing, false otherwise
 */
bool tts_decoder_is_playing(void);

#endif // TTS_DECODER_H

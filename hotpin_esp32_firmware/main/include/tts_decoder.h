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

/**
 * @brief Check if TTS is currently receiving audio data from server
 * 
 * Returns true if the TTS decoder is actively receiving audio chunks via WebSocket.
 * This is useful for preventing mode transitions while audio is being streamed.
 * 
 * @return true if actively receiving audio, false otherwise
 */
bool tts_decoder_is_receiving_audio(void);

/**
 * @brief Query whether buffered audio is still pending playback
 *
 * @return true when queued audio remains to be rendered
 */
bool tts_decoder_has_pending_audio(void);

/**
 * @brief Approximate number of PCM bytes pending playback
 *
 * @return Remaining bytes (best-effort estimate)
 */
size_t tts_decoder_get_pending_bytes(void);

/**
 * @brief Wait until playback completes or timeout expires
 *
 * @param timeout_ms Maximum time to wait in milliseconds
 * @return ESP_OK if drained, ESP_ERR_TIMEOUT on timeout, or error code
 */
esp_err_t tts_decoder_wait_for_idle(uint32_t timeout_ms);

/**
 * @brief Reset TTS decoder session state for next audio stream
 * 
 * Clears session-specific state while keeping decoder running
 */
void tts_decoder_reset_session(void);

/**
 * @brief Notify the decoder that the server finished streaming audio
 *
 * Queues a graceful stop sentinel allowing the playback task to exit
 * after draining buffered audio.
 */
void tts_decoder_notify_end_of_stream(void);

/**
 * @brief Flush and reset TTS decoder completely for session transition
 * 
 * Flushes any pending audio and resets decoder state completely for next session
 * 
 * @return ESP_OK on success
 */
esp_err_t tts_decoder_flush_and_reset(void);

#endif // TTS_DECODER_H

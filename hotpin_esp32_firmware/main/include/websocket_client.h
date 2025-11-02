/**
 * @file websocket_client.h
 * @brief WebSocket client for Hotpin server communication
 * 
 * Handles binary PCM audio transmission and WAV audio reception
 */

#ifndef WEBSOCKET_CLIENT_H
#define WEBSOCKET_CLIENT_H

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ===========================
// Callback Types
// ===========================

/**
 * @brief Callback for incoming binary audio data (TTS WAV)
 * 
 * @param data Audio data buffer
 * @param length Data length in bytes
 * @param arg User-provided argument
 */
typedef void (*websocket_audio_callback_t)(const uint8_t *data, size_t length, void *arg);

/**
 * @brief WebSocket status codes
 */
typedef enum {
    WEBSOCKET_STATUS_CONNECTED,
    WEBSOCKET_STATUS_DISCONNECTED,
    WEBSOCKET_STATUS_ERROR
} websocket_status_t;

typedef enum {
    WEBSOCKET_PIPELINE_STAGE_IDLE = 0,
    WEBSOCKET_PIPELINE_STAGE_TRANSCRIPTION,
    WEBSOCKET_PIPELINE_STAGE_LLM,
    WEBSOCKET_PIPELINE_STAGE_TTS,
    WEBSOCKET_PIPELINE_STAGE_COMPLETE,
    WEBSOCKET_PIPELINE_STAGE_ERROR,  // Added for error state handling
} websocket_pipeline_stage_t;

/**
 * @brief Callback for WebSocket status changes
 * 
 * @param status Current WebSocket status
 * @param arg User-provided argument
 */
typedef void (*websocket_status_callback_t)(websocket_status_t status, void *arg);

// ===========================
// Public Functions
// ===========================

/**
 * @brief Initialize WebSocket client
 * 
 * @param uri WebSocket server URI (e.g., "ws://192.168.1.100:8000/ws")
 * @param auth_token Optional Bearer token for Authorization header (can be NULL)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t websocket_client_init(const char *uri, const char *auth_token);

/**
 * @brief Deinitialize WebSocket client
 * 
 * @return ESP_OK on success
 */
esp_err_t websocket_client_deinit(void);

/**
 * @brief Connect to WebSocket server
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t websocket_client_connect(void);

/**
 * @brief Disconnect from WebSocket server
 * 
 * @return ESP_OK on success
 */
esp_err_t websocket_client_disconnect(void);

/**
 * @brief Force-stop the WebSocket client regardless of connection state
 *
 * Useful for shutdown paths where the remote side has already disconnected and
 * the client may report ESP_FAIL when attempting a graceful stop.
 *
 * @return ESP_OK on success or if the client was already stopped
 */
esp_err_t websocket_client_force_stop(void);

/**
 * @brief Send binary PCM audio data
 * 
 * @param data PCM audio buffer
 * @param length Buffer length in bytes
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t websocket_client_send_audio(const uint8_t *data, size_t length, uint32_t timeout_ms);

/**
 * @brief Send text message (JSON)
 * 
 * @param message Text message to send
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t websocket_client_send_text(const char *message);

/**
 * @brief Send session handshake to server
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t websocket_client_send_handshake(void);

/**
 * @brief Send End-of-Stream (EOS) signal
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t websocket_client_send_eos(void);

/**
 * @brief Check if WebSocket is connected
 * 
 * @return true if connected, false otherwise
 */
bool websocket_client_is_connected(void);
bool websocket_client_session_ready(void);
bool websocket_client_can_stream_audio(void);

/**
 * @brief Register callback for incoming audio data (TTS)
 * 
 * @param callback Function to call when audio data is received
 * @param arg User argument passed to callback
 */
void websocket_client_set_audio_callback(websocket_audio_callback_t callback, void *arg);

/**
 * @brief Register callback for WebSocket status changes
 * 
 * @param callback Function to call on status changes
 * @param arg User argument passed to callback
 */
void websocket_client_set_status_callback(websocket_status_callback_t callback, void *arg);

websocket_pipeline_stage_t websocket_client_get_pipeline_stage(void);
bool websocket_client_is_pipeline_active(void);
const char *websocket_client_pipeline_stage_to_string(websocket_pipeline_stage_t stage);

#endif // WEBSOCKET_CLIENT_H

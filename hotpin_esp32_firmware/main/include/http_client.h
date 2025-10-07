/**
 * @file http_client.h
 * @brief HTTP client for camera image upload with multipart/form-data
 * 
 * Provides blocking image upload with Authorization bearer token
 */

#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

/**
 * @brief Initialize HTTP client
 * 
 * @param server_url Base server URL (e.g., "http://192.168.1.100:8000")
 * @param auth_token Bearer token for Authorization header (can be NULL)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t http_client_init(const char *server_url, const char *auth_token);

/**
 * @brief Deinitialize HTTP client
 * 
 * @return ESP_OK on success
 */
esp_err_t http_client_deinit(void);

/**
 * @brief Upload JPEG image to server
 * 
 * Sends multipart/form-data POST to /image endpoint:
 * - session: session ID string
 * - file: JPEG binary data
 * 
 * @param session_id Session identifier string
 * @param jpeg_data Pointer to JPEG buffer
 * @param jpeg_len Length of JPEG data
 * @param response_buffer Buffer to store server response (optional, can be NULL)
 * @param response_buffer_size Size of response buffer
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t http_client_upload_image(const char *session_id,
                                     const uint8_t *jpeg_data,
                                     size_t jpeg_len,
                                     char *response_buffer,
                                     size_t response_buffer_size);

#endif // HTTP_CLIENT_H

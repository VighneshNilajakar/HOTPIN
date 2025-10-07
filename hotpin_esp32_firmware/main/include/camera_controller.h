/**
 * @file camera_controller.h
 * @brief OV2640 camera controller for ESP32-CAM
 * 
 * Manages camera initialization, frame capture, and mode switching
 */

#ifndef CAMERA_CONTROLLER_H
#define CAMERA_CONTROLLER_H

#include "esp_err.h"
#include "esp_camera.h"

/**
 * @brief Initialize camera with AI-Thinker pin configuration
 * 
 * Configures OV2640 sensor with PSRAM-backed frame buffers
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t camera_controller_init(void);

/**
 * @brief Deinitialize camera (for mode switching)
 * 
 * Cleanly releases camera resources before I2S initialization
 * 
 * @return ESP_OK on success
 */
esp_err_t camera_controller_deinit(void);

/**
 * @brief Capture a single frame
 * 
 * @return Pointer to frame buffer (must be released with camera_fb_return)
 */
camera_fb_t* camera_controller_capture_frame(void);

/**
 * @brief Check if camera is initialized
 * 
 * @return true if initialized, false otherwise
 */
bool camera_controller_is_initialized(void);

#endif // CAMERA_CONTROLLER_H

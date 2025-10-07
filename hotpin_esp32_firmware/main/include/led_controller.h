/**
 * @file led_controller.h
 * @brief LED status indicator controller
 * 
 * Provides visual feedback for system states:
 * - WiFi: Blinking while connecting, solid when connected
 * - Recording: Solid during voice capture
 * - Playback: Pulsing during TTS playback
 * - Error: Fast blinking on errors
 */

#ifndef LED_CONTROLLER_H
#define LED_CONTROLLER_H

#include "esp_err.h"
#include <stdbool.h>

typedef enum {
    LED_STATE_OFF = 0,
    LED_STATE_WIFI_CONNECTING,      // Slow blink (500ms on/off)
    LED_STATE_WIFI_CONNECTED,       // Solid on
    LED_STATE_RECORDING,            // Solid on (bright)
    LED_STATE_PLAYBACK,             // Pulsing (PWM fade in/out)
    LED_STATE_ERROR                 // Fast blink (100ms on/off)
} led_state_t;

/**
 * @brief Initialize LED controller
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t led_controller_init(void);

/**
 * @brief Deinitialize LED controller
 * 
 * @return ESP_OK on success
 */
esp_err_t led_controller_deinit(void);

/**
 * @brief Set LED state
 * 
 * @param state Desired LED state
 * @return ESP_OK on success
 */
esp_err_t led_controller_set_state(led_state_t state);

/**
 * @brief Get current LED state
 * 
 * @return Current LED state
 */
led_state_t led_controller_get_state(void);

#endif // LED_CONTROLLER_H

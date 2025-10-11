/**
 * @file led_controller.h
 * @brief LED status indicator controller
 * 
 * Provides non-blocking LED patterns for system feedback:
 * - Fast blink during boot/connection
 * - Breathing pulse while idle/standby
 * - Solid on for active voice capture
 * - Rhythmic pulsing while processing
 * - SOS cadence for critical faults
 * - Single flash for capture events
 */

#ifndef LED_CONTROLLER_H
#define LED_CONTROLLER_H

#include "esp_err.h"
#include <stdbool.h>

typedef enum {
    LED_STATE_OFF = 0,
    LED_STATE_SOLID,
    LED_STATE_FAST_BLINK,
    LED_STATE_BREATHING,
    LED_STATE_PULSING,
    LED_STATE_SOS,
    LED_STATE_FLASH
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

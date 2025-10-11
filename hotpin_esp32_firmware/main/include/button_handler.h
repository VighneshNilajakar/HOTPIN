/**
 * @file button_handler.h
 * @brief Push button handler with FSM for click pattern detection
 * 
 * Implements:
 * - GPIO ISR with edge detection
 * - Software debouncing (50ms)
 * - Single-click detection (deferred until double-click window expires)
 * - Long-press detection (>3000ms)
 * - Event dispatcher integration for state manager communication
 */

#ifndef BUTTON_HANDLER_H
#define BUTTON_HANDLER_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "system_events.h"

// ===========================
// Button FSM States
// ===========================
typedef enum {
    BUTTON_STATE_IDLE = 0,           // Button released, no activity
    BUTTON_STATE_DEBOUNCE_PRESS,     // Debouncing press event
    BUTTON_STATE_PRESSED,            // Button confirmed pressed
    BUTTON_STATE_WAIT_RELEASE,       // Waiting for button release
    BUTTON_STATE_DEBOUNCE_RELEASE,   // Debouncing release event
    BUTTON_STATE_LONG_PRESS          // Long press detected (>3000ms)
} button_state_t;

// ===========================
// Public API
// ===========================

/**
 * @brief Initialize button handler
 * 
 * Configures GPIO interrupt, creates FSM task, and binds to system event dispatcher
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t button_handler_init(void);

/**
 * @brief Deinitialize button handler
 * 
 * Cleans up GPIO ISR, stops task, and releases resources
 * 
 * @return ESP_OK on success
 */
esp_err_t button_handler_deinit(void);

/**
 * @brief Get current button state
 * 
 * @return Current FSM state
 */
button_state_t button_handler_get_state(void);

/**
 * @brief Get button press count (for debugging)
 * 
 * @return Total number of button presses since init
 */
uint32_t button_handler_get_press_count(void);

/**
 * @brief Reset button FSM to idle state
 * 
 * Used for error recovery or manual reset
 */
void button_handler_reset(void);

/**
 * @brief Returns true if the shared GPIO ISR service has been installed.
 */
bool button_handler_isr_service_installed(void);

#endif // BUTTON_HANDLER_H

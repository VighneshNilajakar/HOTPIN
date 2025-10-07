/**
 * @file button_handler.h
 * @brief Push button handler with FSM for click pattern detection
 * 
 * Implements:
 * - GPIO ISR with edge detection
 * - Software debouncing (50ms)
 * - Single-click detection (deferred until double-click window expires)
 * - Long-press detection (>3000ms)
 * - Event queue for state manager communication
 */

#ifndef BUTTON_HANDLER_H
#define BUTTON_HANDLER_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "driver/gpio.h"

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
// Button Event Types
// ===========================
typedef enum {
    BUTTON_EVENT_NONE = 0,
    BUTTON_EVENT_SINGLE_CLICK,       // Single click confirmed
    BUTTON_EVENT_DOUBLE_CLICK,       // Double click detected
    BUTTON_EVENT_LONG_PRESS,         // Long press (>3000ms)
    BUTTON_EVENT_LONG_PRESS_RELEASE  // Long press released
} button_event_type_t;

// ===========================
// Button Event Structure
// ===========================
typedef struct {
    button_event_type_t type;
    uint32_t timestamp_ms;           // Event timestamp
    uint32_t duration_ms;            // Press duration (for long press)
} button_event_t;

// ===========================
// Public API
// ===========================

/**
 * @brief Initialize button handler
 * 
 * Configures GPIO interrupt, creates FSM task, and sets up event queue
 * 
 * @param event_queue_handle FreeRTOS queue for posting button events
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t button_handler_init(QueueHandle_t event_queue_handle);

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

#endif // BUTTON_HANDLER_H

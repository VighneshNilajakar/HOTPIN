/**
 * @file state_manager.h
 * @brief System state manager with FSM for camera/voice mode switching
 * 
 * Coordinates driver transitions with mutex-protected switching
 */

#ifndef STATE_MANAGER_H
#define STATE_MANAGER_H

#include "esp_err.h"
#include <stdint.h>

// ===========================
// System States
// ===========================
typedef enum {
    SYSTEM_STATE_INIT = 0,
    SYSTEM_STATE_CAMERA_STANDBY,
    SYSTEM_STATE_VOICE_ACTIVE,
    SYSTEM_STATE_TRANSITIONING,
    SYSTEM_STATE_ERROR,
    SYSTEM_STATE_SHUTDOWN
} system_state_t;

// ===========================
// Public API
// ===========================

/**
 * @brief State manager task entry point
 * 
 * Main FSM task running on Core 1
 * 
 * @param pvParameters Task parameters (unused)
 */
void state_manager_task(void *pvParameters);

/**
 * @brief Get current system state
 * 
 * @return Current system state
 */
system_state_t state_manager_get_state(void);

#endif // STATE_MANAGER_H

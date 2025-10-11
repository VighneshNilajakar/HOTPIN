/**
 * @file serial_commands.h
 * @brief Serial command interface for debugging and testing
 * 
 * Provides UART-based command interface for testing system functionality
 * without requiring physical button presses.
 */

#ifndef SERIAL_COMMANDS_H
#define SERIAL_COMMANDS_H

#include "esp_err.h"

/**
 * @brief Initialize serial command handler
 *
 * @return ESP_OK on success
 */
esp_err_t serial_commands_init(void);

/**
 * @brief Deinitialize serial command handler
 * 
 * @return ESP_OK on success
 */
esp_err_t serial_commands_deinit(void);

#endif // SERIAL_COMMANDS_H

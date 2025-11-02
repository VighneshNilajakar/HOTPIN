/**
 * @file memory_manager.h
 * @brief Comprehensive memory management and monitoring system for HotPin
 */

#ifndef MEMORY_MANAGER_H
#define MEMORY_MANAGER_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ===========================
// Constants
// ===========================

#define MEMORY_MONITOR_MIN_INTERVAL_MS      5000    // Minimum monitoring interval
#define MEMORY_MONITOR_DEFAULT_INTERVAL_MS  10000   // Default 10 seconds
#define MAX_WARNING_CALLBACKS               5       // Maximum warning callbacks

// ===========================
// Type Definitions
// ===========================

/**
 * @brief Memory statistics structure
 */
typedef struct {
    uint32_t timestamp_ms;              // Timestamp of measurement
    
    // Internal RAM (DRAM)
    uint32_t internal_free;             // Free internal RAM
    uint32_t internal_largest;          // Largest contiguous block
    uint32_t internal_fragmentation;    // Fragmentation percentage (0-100)
    
    // DMA-capable RAM
    uint32_t dma_free;                  // Free DMA-capable memory
    uint32_t dma_largest;               // Largest contiguous DMA block
    uint32_t dma_fragmentation;         // DMA fragmentation percentage
    
    // PSRAM (External RAM)
    uint32_t psram_free;                // Free PSRAM
    uint32_t psram_largest;             // Largest contiguous PSRAM block
    uint32_t psram_fragmentation;       // PSRAM fragmentation percentage
    
    // Total heap
    uint32_t total_free;                // Total free heap
    uint32_t total_minimum_free;        // Minimum free heap ever recorded
} memory_stats_t;

/**
 * @brief Memory threshold configuration
 */
typedef struct {
    uint32_t internal_ram_warning;      // Internal RAM warning threshold (bytes)
    uint32_t internal_ram_critical;     // Internal RAM critical threshold (bytes)
    uint32_t dma_capable_warning;       // DMA memory warning threshold (bytes)
    uint32_t dma_capable_critical;      // DMA memory critical threshold (bytes)
    uint32_t psram_warning;             // PSRAM warning threshold (bytes)
    uint32_t psram_critical;            // PSRAM critical threshold (bytes)
    uint32_t total_heap_warning;        // Total heap warning threshold (bytes)
    uint32_t total_heap_critical;       // Total heap critical threshold (bytes)
    uint32_t fragmentation_warning;     // Fragmentation warning threshold (percentage)
    uint32_t fragmentation_critical;    // Fragmentation critical threshold (percentage)
} memory_thresholds_t;

/**
 * @brief Memory warning types
 */
typedef enum {
    MEMORY_WARNING_INTERNAL_LOW,        // Internal RAM below warning threshold
    MEMORY_WARNING_INTERNAL_CRITICAL,   // Internal RAM critically low
    MEMORY_WARNING_DMA_LOW,             // DMA memory below warning threshold
    MEMORY_WARNING_DMA_CRITICAL,        // DMA memory critically low
    MEMORY_WARNING_PSRAM_LOW,           // PSRAM below warning threshold
    MEMORY_WARNING_PSRAM_CRITICAL,      // PSRAM critically low
    MEMORY_WARNING_FRAGMENTATION_HIGH,  // Memory fragmentation high
    MEMORY_WARNING_LEAK_DETECTED,       // Potential memory leak detected
} memory_warning_type_t;

/**
 * @brief Memory warning information
 */
typedef struct {
    memory_warning_type_t type;         // Warning type
    uint32_t timestamp_ms;              // When warning occurred
    uint32_t current_value;             // Current memory value (bytes or percentage)
    uint32_t threshold_value;           // Threshold that triggered warning
} memory_warning_t;

/**
 * @brief Memory warning callback function type
 * 
 * @param warning Pointer to warning information
 */
typedef void (*memory_warning_callback_t)(const memory_warning_t *warning);

// ===========================
// Public Functions
// ===========================

/**
 * @brief Initialize memory manager
 * 
 * @param thresholds Pointer to threshold configuration (NULL for defaults)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t memory_manager_init(const memory_thresholds_t *thresholds);

/**
 * @brief Start periodic memory monitoring
 * 
 * Creates a background task that monitors memory usage and triggers warnings
 * when thresholds are exceeded.
 * 
 * @param interval_ms Monitoring interval in milliseconds (minimum 5000ms)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t memory_manager_start_monitoring(uint32_t interval_ms);

/**
 * @brief Stop memory monitoring
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t memory_manager_stop_monitoring(void);

/**
 * @brief Get current memory statistics
 * 
 * @param stats Pointer to structure to receive statistics
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t memory_manager_get_stats(memory_stats_t *stats);

/**
 * @brief Register a callback for memory warnings
 * 
 * @param callback Function to call when memory warnings occur
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t memory_manager_register_warning_callback(memory_warning_callback_t callback);

/**
 * @brief Log current memory statistics
 * 
 * Logs detailed memory information to console.
 * 
 * @param context Optional context string for log message (NULL for default)
 */
void memory_manager_log_stats(const char *context);

/**
 * @brief Get free DMA-capable memory
 * 
 * @return Free DMA memory in bytes
 */
uint32_t memory_manager_get_free_dma(void);

/**
 * @brief Get free PSRAM
 * 
 * @return Free PSRAM in bytes
 */
uint32_t memory_manager_get_free_psram(void);

/**
 * @brief Get free internal RAM
 * 
 * @return Free internal RAM in bytes
 */
uint32_t memory_manager_get_free_internal(void);

/**
 * @brief Check if sufficient DMA memory is available
 * 
 * @param required_bytes Minimum required bytes
 * @return true if available, false otherwise
 */
bool memory_manager_check_dma_available(size_t required_bytes);

/**
 * @brief Check if sufficient PSRAM is available
 * 
 * @param required_bytes Minimum required bytes
 * @return true if available, false otherwise
 */
bool memory_manager_check_psram_available(size_t required_bytes);

/**
 * @brief Perform memory optimization
 * 
 * Analyzes current memory state and performs optimization if needed.
 * Logs recommendations for actions that need to be taken by components.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t memory_manager_optimize(void);

#ifdef __cplusplus
}
#endif

#endif // MEMORY_MANAGER_H

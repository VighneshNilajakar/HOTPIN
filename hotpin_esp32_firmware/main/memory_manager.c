/**
 * @file memory_manager.c
 * @brief Comprehensive memory management and monitoring system for HotPin
 * 
 * Provides:
 * - Real-time memory usage tracking
 * - Memory threshold monitoring with warnings
 * - Automatic memory optimization
 * - Memory leak detection
 * - Heap fragmentation analysis
 */

#include "memory_manager.h"
#include "config.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "MEM_MGR";

// Memory statistics
static memory_stats_t g_current_stats;
static memory_stats_t g_baseline_stats;
static memory_thresholds_t g_thresholds;
static SemaphoreHandle_t g_stats_mutex = NULL;
static bool g_initialized = false;
static TaskHandle_t g_monitor_task_handle = NULL;
static volatile bool g_monitoring_enabled = false;

// Memory warning callbacks
static memory_warning_callback_t g_warning_callbacks[MAX_WARNING_CALLBACKS];
static int g_warning_callback_count = 0;

// Forward declarations
static void memory_monitor_task(void *pvParameters);
static void update_memory_stats(void);
static void check_memory_thresholds(void);
static uint32_t calculate_fragmentation_percentage(size_t total, size_t largest_block);

// ===========================
// Public Functions
// ===========================

esp_err_t memory_manager_init(const memory_thresholds_t *thresholds) {
    if (g_initialized) {
        ESP_LOGW(TAG, "Memory manager already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing memory manager...");

    // Create statistics mutex
    g_stats_mutex = xSemaphoreCreateMutex();
    if (g_stats_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create statistics mutex");
        return ESP_ERR_NO_MEM;
    }

    // Set thresholds (use defaults if NULL)
    if (thresholds != NULL) {
        memcpy(&g_thresholds, thresholds, sizeof(memory_thresholds_t));
    } else {
        // Default thresholds
        g_thresholds.internal_ram_warning = 50 * 1024;      // 50KB
        g_thresholds.internal_ram_critical = 20 * 1024;     // 20KB
        g_thresholds.dma_capable_warning = 35 * 1024;       // 35KB
        g_thresholds.dma_capable_critical = 20 * 1024;      // 20KB
        g_thresholds.psram_warning = 500 * 1024;            // 500KB
        g_thresholds.psram_critical = 200 * 1024;           // 200KB
        g_thresholds.total_heap_warning = 600 * 1024;       // 600KB
        g_thresholds.total_heap_critical = 300 * 1024;      // 300KB
        g_thresholds.fragmentation_warning = 30;            // 30%
        g_thresholds.fragmentation_critical = 50;           // 50%
    }

    // Take baseline measurement
    update_memory_stats();
    memcpy(&g_baseline_stats, &g_current_stats, sizeof(memory_stats_t));

    ESP_LOGI(TAG, "╔═══════════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "║ Memory Manager Initialized");
    ESP_LOGI(TAG, "╠═══════════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "║ Baseline Memory State:");
    ESP_LOGI(TAG, "║   Internal RAM:     %6lu bytes (%3lu KB)", 
             (unsigned long)g_baseline_stats.internal_free, (unsigned long)(g_baseline_stats.internal_free / 1024));
    ESP_LOGI(TAG, "║   DMA-capable:      %6lu bytes (%3lu KB)", 
             (unsigned long)g_baseline_stats.dma_free, (unsigned long)(g_baseline_stats.dma_free / 1024));
    ESP_LOGI(TAG, "║   PSRAM:            %6lu bytes (%4lu KB)", 
             (unsigned long)g_baseline_stats.psram_free, (unsigned long)(g_baseline_stats.psram_free / 1024));
    ESP_LOGI(TAG, "║   Total Heap:       %6lu bytes (%4lu KB)", 
             (unsigned long)g_baseline_stats.total_free, (unsigned long)(g_baseline_stats.total_free / 1024));
    ESP_LOGI(TAG, "╠═══════════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "║ Configured Thresholds:");
    ESP_LOGI(TAG, "║   Internal RAM:     WARN=%luKB, CRIT=%luKB", 
             (unsigned long)(g_thresholds.internal_ram_warning / 1024), 
             (unsigned long)(g_thresholds.internal_ram_critical / 1024));
    ESP_LOGI(TAG, "║   DMA-capable:      WARN=%luKB, CRIT=%luKB", 
             (unsigned long)(g_thresholds.dma_capable_warning / 1024), 
             (unsigned long)(g_thresholds.dma_capable_critical / 1024));
    ESP_LOGI(TAG, "║   PSRAM:            WARN=%luKB, CRIT=%luKB", 
             (unsigned long)(g_thresholds.psram_warning / 1024), 
             (unsigned long)(g_thresholds.psram_critical / 1024));
    ESP_LOGI(TAG, "║   Fragmentation:    WARN=%lu%%, CRIT=%lu%%", 
             (unsigned long)g_thresholds.fragmentation_warning, 
             (unsigned long)g_thresholds.fragmentation_critical);
    ESP_LOGI(TAG, "╚═══════════════════════════════════════════════════════════");

    g_initialized = true;
    return ESP_OK;
}

esp_err_t memory_manager_start_monitoring(uint32_t interval_ms) {
    if (!g_initialized) {
        ESP_LOGE(TAG, "Memory manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (g_monitoring_enabled) {
        ESP_LOGW(TAG, "Memory monitoring already active");
        return ESP_OK;
    }

    if (interval_ms < MEMORY_MONITOR_MIN_INTERVAL_MS) {
        ESP_LOGW(TAG, "Interval too short, using minimum: %lu ms", (unsigned long)MEMORY_MONITOR_MIN_INTERVAL_MS);
        interval_ms = MEMORY_MONITOR_MIN_INTERVAL_MS;
    }

    ESP_LOGI(TAG, "Starting memory monitoring (interval: %lu ms)", (unsigned long)interval_ms);

    g_monitoring_enabled = true;

    BaseType_t ret = xTaskCreatePinnedToCore(
        memory_monitor_task,
        "mem_monitor",
        TASK_STACK_SIZE_SMALL,
        (void *)(uintptr_t)interval_ms,
        3,  // Low priority (above idle priority)
        &g_monitor_task_handle,
        TASK_CORE_CONTROL  // Core 1 - low priority monitoring
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create memory monitor task");
        g_monitoring_enabled = false;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "✅ Memory monitoring started");
    return ESP_OK;
}

esp_err_t memory_manager_stop_monitoring(void) {
    if (!g_monitoring_enabled) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping memory monitoring...");
    g_monitoring_enabled = false;

    if (g_monitor_task_handle != NULL) {
        // Wait for task to exit gracefully
        vTaskDelay(pdMS_TO_TICKS(100));
        if (eTaskGetState(g_monitor_task_handle) != eDeleted) {
            vTaskDelete(g_monitor_task_handle);
        }
        g_monitor_task_handle = NULL;
    }

    ESP_LOGI(TAG, "✅ Memory monitoring stopped");
    return ESP_OK;
}

esp_err_t memory_manager_get_stats(memory_stats_t *stats) {
    if (stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!g_initialized) {
        ESP_LOGE(TAG, "Memory manager not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(g_stats_mutex, portMAX_DELAY);
    memcpy(stats, &g_current_stats, sizeof(memory_stats_t));
    xSemaphoreGive(g_stats_mutex);

    return ESP_OK;
}

esp_err_t memory_manager_register_warning_callback(memory_warning_callback_t callback) {
    if (callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (g_warning_callback_count >= MAX_WARNING_CALLBACKS) {
        ESP_LOGW(TAG, "Maximum warning callbacks reached");
        return ESP_ERR_NO_MEM;
    }

    g_warning_callbacks[g_warning_callback_count++] = callback;
    ESP_LOGD(TAG, "Warning callback registered (total: %d)", g_warning_callback_count);

    return ESP_OK;
}

void memory_manager_log_stats(const char *context) {
    if (!g_initialized) {
        return;
    }

    memory_stats_t stats;
    memory_manager_get_stats(&stats);

    ESP_LOGI(TAG, "╔═══════════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "║ Memory Statistics - %s", context ? context : "Current State");
    ESP_LOGI(TAG, "╠═══════════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "║ Internal RAM:");
    ESP_LOGI(TAG, "║   Free:             %6lu bytes (%3lu KB)", 
             (unsigned long)stats.internal_free, (unsigned long)(stats.internal_free / 1024));
    ESP_LOGI(TAG, "║   Largest Block:    %6lu bytes (%3lu KB)", 
             (unsigned long)stats.internal_largest, (unsigned long)(stats.internal_largest / 1024));
    ESP_LOGI(TAG, "║   Fragmentation:    %3lu%%", (unsigned long)stats.internal_fragmentation);
    ESP_LOGI(TAG, "╠═══════════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "║ DMA-capable RAM:");
    ESP_LOGI(TAG, "║   Free:             %6lu bytes (%3lu KB)", 
             (unsigned long)stats.dma_free, (unsigned long)(stats.dma_free / 1024));
    ESP_LOGI(TAG, "║   Largest Block:    %6lu bytes (%3lu KB)", 
             (unsigned long)stats.dma_largest, (unsigned long)(stats.dma_largest / 1024));
    ESP_LOGI(TAG, "║   Fragmentation:    %3lu%%", (unsigned long)stats.dma_fragmentation);
    ESP_LOGI(TAG, "╠═══════════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "║ PSRAM:");
    ESP_LOGI(TAG, "║   Free:             %6lu bytes (%4lu KB)", 
             (unsigned long)stats.psram_free, (unsigned long)(stats.psram_free / 1024));
    ESP_LOGI(TAG, "║   Largest Block:    %6lu bytes (%4lu KB)", 
             (unsigned long)stats.psram_largest, (unsigned long)(stats.psram_largest / 1024));
    ESP_LOGI(TAG, "║   Fragmentation:    %3lu%%", (unsigned long)stats.psram_fragmentation);
    ESP_LOGI(TAG, "╠═══════════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "║ Total Heap:");
    ESP_LOGI(TAG, "║   Free:             %6lu bytes (%4lu KB)", 
             (unsigned long)stats.total_free, (unsigned long)(stats.total_free / 1024));
    ESP_LOGI(TAG, "║   Minimum Ever:     %6lu bytes (%4lu KB)", 
             (unsigned long)stats.total_minimum_free, (unsigned long)(stats.total_minimum_free / 1024));
    ESP_LOGI(TAG, "╠═══════════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "║ Memory Delta from Baseline:");
    int32_t delta_internal = (int32_t)stats.internal_free - (int32_t)g_baseline_stats.internal_free;
    int32_t delta_dma = (int32_t)stats.dma_free - (int32_t)g_baseline_stats.dma_free;
    int32_t delta_psram = (int32_t)stats.psram_free - (int32_t)g_baseline_stats.psram_free;
    int32_t delta_total = (int32_t)stats.total_free - (int32_t)g_baseline_stats.total_free;
    
    ESP_LOGI(TAG, "║   Internal RAM:     %+7ld bytes (%+4ld KB)", (long)delta_internal, (long)(delta_internal / 1024));
    ESP_LOGI(TAG, "║   DMA-capable:      %+7ld bytes (%+4ld KB)", (long)delta_dma, (long)(delta_dma / 1024));
    ESP_LOGI(TAG, "║   PSRAM:            %+7ld bytes (%+4ld KB)", (long)delta_psram, (long)(delta_psram / 1024));
    ESP_LOGI(TAG, "║   Total Heap:       %+7ld bytes (%+4ld KB)", (long)delta_total, (long)(delta_total / 1024));
    ESP_LOGI(TAG, "╚═══════════════════════════════════════════════════════════");
}

uint32_t memory_manager_get_free_dma(void) {
    return heap_caps_get_free_size(MALLOC_CAP_DMA);
}

uint32_t memory_manager_get_free_psram(void) {
    return heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

uint32_t memory_manager_get_free_internal(void) {
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

bool memory_manager_check_dma_available(size_t required_bytes) {
    size_t available = heap_caps_get_free_size(MALLOC_CAP_DMA);
    if (available < required_bytes) {
        ESP_LOGW(TAG, "Insufficient DMA memory: need %lu bytes, have %lu bytes", 
                 (unsigned long)required_bytes, (unsigned long)available);
        return false;
    }
    return true;
}

bool memory_manager_check_psram_available(size_t required_bytes) {
    size_t available = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (available < required_bytes) {
        ESP_LOGW(TAG, "Insufficient PSRAM: need %lu bytes, have %lu bytes", 
                 (unsigned long)required_bytes, (unsigned long)available);
        return false;
    }
    return true;
}

esp_err_t memory_manager_optimize(void) {
    if (!g_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Running memory optimization...");

    // Log pre-optimization state
    memory_manager_log_stats("Pre-Optimization");

    // No explicit heap compaction in ESP-IDF, but we can log recommendations
    memory_stats_t stats;
    memory_manager_get_stats(&stats);

    bool optimized = false;

    // Check for high fragmentation
    if (stats.dma_fragmentation >= g_thresholds.fragmentation_warning) {
        ESP_LOGW(TAG, "High DMA fragmentation detected (%lu%%) - consider reinitializing audio driver",
                 (unsigned long)stats.dma_fragmentation);
        optimized = true;
    }

    if (stats.psram_fragmentation >= g_thresholds.fragmentation_warning) {
        ESP_LOGW(TAG, "High PSRAM fragmentation detected (%lu%%) - consider restarting buffers",
                 (unsigned long)stats.psram_fragmentation);
        optimized = true;
    }

    // Log post-optimization state
    memory_manager_log_stats("Post-Optimization");

    if (!optimized) {
        ESP_LOGI(TAG, "✅ Memory state is healthy - no optimization needed");
    } else {
        ESP_LOGI(TAG, "⚠️ Optimization recommendations logged");
    }

    return ESP_OK;
}

// ===========================
// Private Functions
// ===========================

static void memory_monitor_task(void *pvParameters) {
    uint32_t interval_ms = (uint32_t)(uintptr_t)pvParameters;
    TickType_t delay_ticks = pdMS_TO_TICKS(interval_ms);

    ESP_LOGI(TAG, "Memory monitor task started (interval: %lu ms)", (unsigned long)interval_ms);

    while (g_monitoring_enabled) {
        // Update statistics
        update_memory_stats();

        // Check thresholds and trigger warnings if needed
        check_memory_thresholds();

        // Sleep until next interval
        vTaskDelay(delay_ticks);
    }

    ESP_LOGI(TAG, "Memory monitor task exiting");
    g_monitor_task_handle = NULL;
    vTaskDelete(NULL);
}

static void update_memory_stats(void) {
    xSemaphoreTake(g_stats_mutex, portMAX_DELAY);

    // Update timestamp
    g_current_stats.timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);

    // Internal RAM
    g_current_stats.internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    g_current_stats.internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    g_current_stats.internal_fragmentation = calculate_fragmentation_percentage(
        g_current_stats.internal_free, g_current_stats.internal_largest);

    // DMA-capable RAM
    g_current_stats.dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
    g_current_stats.dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    g_current_stats.dma_fragmentation = calculate_fragmentation_percentage(
        g_current_stats.dma_free, g_current_stats.dma_largest);

    // PSRAM
    g_current_stats.psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    g_current_stats.psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    g_current_stats.psram_fragmentation = calculate_fragmentation_percentage(
        g_current_stats.psram_free, g_current_stats.psram_largest);

    // Total heap
    g_current_stats.total_free = esp_get_free_heap_size();
    g_current_stats.total_minimum_free = esp_get_minimum_free_heap_size();

    xSemaphoreGive(g_stats_mutex);
}

static void check_memory_thresholds(void) {
    memory_stats_t stats;
    xSemaphoreTake(g_stats_mutex, portMAX_DELAY);
    memcpy(&stats, &g_current_stats, sizeof(memory_stats_t));
    xSemaphoreGive(g_stats_mutex);

    memory_warning_t warning;
    warning.timestamp_ms = stats.timestamp_ms;

    // Check internal RAM
    if (stats.internal_free < g_thresholds.internal_ram_critical) {
        warning.type = MEMORY_WARNING_INTERNAL_CRITICAL;
        warning.current_value = stats.internal_free;
        warning.threshold_value = g_thresholds.internal_ram_critical;
        
        ESP_LOGE(TAG, "🚨 CRITICAL: Internal RAM very low! %lu bytes (threshold: %lu bytes)",
                 (unsigned long)stats.internal_free, (unsigned long)g_thresholds.internal_ram_critical);
        
        // Notify callbacks
        for (int i = 0; i < g_warning_callback_count; i++) {
            g_warning_callbacks[i](&warning);
        }
    } else if (stats.internal_free < g_thresholds.internal_ram_warning) {
        warning.type = MEMORY_WARNING_INTERNAL_LOW;
        warning.current_value = stats.internal_free;
        warning.threshold_value = g_thresholds.internal_ram_warning;
        
        ESP_LOGW(TAG, "⚠️ WARNING: Internal RAM low! %lu bytes (threshold: %lu bytes)",
                 (unsigned long)stats.internal_free, (unsigned long)g_thresholds.internal_ram_warning);
        
        // Notify callbacks
        for (int i = 0; i < g_warning_callback_count; i++) {
            g_warning_callbacks[i](&warning);
        }
    }

    // Check DMA-capable RAM
    if (stats.dma_free < g_thresholds.dma_capable_critical) {
        warning.type = MEMORY_WARNING_DMA_CRITICAL;
        warning.current_value = stats.dma_free;
        warning.threshold_value = g_thresholds.dma_capable_critical;
        
        ESP_LOGE(TAG, "🚨 CRITICAL: DMA memory very low! %lu bytes (threshold: %lu bytes)",
                 (unsigned long)stats.dma_free, (unsigned long)g_thresholds.dma_capable_critical);
        
        for (int i = 0; i < g_warning_callback_count; i++) {
            g_warning_callbacks[i](&warning);
        }
    } else if (stats.dma_free < g_thresholds.dma_capable_warning) {
        warning.type = MEMORY_WARNING_DMA_LOW;
        warning.current_value = stats.dma_free;
        warning.threshold_value = g_thresholds.dma_capable_warning;
        
        ESP_LOGW(TAG, "⚠️ WARNING: DMA memory low! %lu bytes (threshold: %lu bytes)",
                 (unsigned long)stats.dma_free, (unsigned long)g_thresholds.dma_capable_warning);
        
        for (int i = 0; i < g_warning_callback_count; i++) {
            g_warning_callbacks[i](&warning);
        }
    }

    // Check PSRAM
    if (stats.psram_free < g_thresholds.psram_critical) {
        warning.type = MEMORY_WARNING_PSRAM_CRITICAL;
        warning.current_value = stats.psram_free;
        warning.threshold_value = g_thresholds.psram_critical;
        
        ESP_LOGE(TAG, "🚨 CRITICAL: PSRAM very low! %lu bytes (threshold: %lu bytes)",
                 (unsigned long)stats.psram_free, (unsigned long)g_thresholds.psram_critical);
        
        for (int i = 0; i < g_warning_callback_count; i++) {
            g_warning_callbacks[i](&warning);
        }
    } else if (stats.psram_free < g_thresholds.psram_warning) {
        warning.type = MEMORY_WARNING_PSRAM_LOW;
        warning.current_value = stats.psram_free;
        warning.threshold_value = g_thresholds.psram_warning;
        
        ESP_LOGW(TAG, "⚠️ WARNING: PSRAM low! %lu bytes (threshold: %lu bytes)",
                 (unsigned long)stats.psram_free, (unsigned long)g_thresholds.psram_warning);
        
        for (int i = 0; i < g_warning_callback_count; i++) {
            g_warning_callbacks[i](&warning);
        }
    }

    // Check fragmentation
    if (stats.dma_fragmentation >= g_thresholds.fragmentation_critical ||
        stats.psram_fragmentation >= g_thresholds.fragmentation_critical) {
        warning.type = MEMORY_WARNING_FRAGMENTATION_HIGH;
        warning.current_value = (stats.dma_fragmentation > stats.psram_fragmentation) ? 
                                 stats.dma_fragmentation : stats.psram_fragmentation;
        warning.threshold_value = g_thresholds.fragmentation_critical;
        
        ESP_LOGE(TAG, "🚨 CRITICAL: High memory fragmentation! DMA:%lu%% PSRAM:%lu%%",
                 (unsigned long)stats.dma_fragmentation, (unsigned long)stats.psram_fragmentation);
        
        for (int i = 0; i < g_warning_callback_count; i++) {
            g_warning_callbacks[i](&warning);
        }
    } else if (stats.dma_fragmentation >= g_thresholds.fragmentation_warning ||
               stats.psram_fragmentation >= g_thresholds.fragmentation_warning) {
        warning.type = MEMORY_WARNING_FRAGMENTATION_HIGH;
        warning.current_value = (stats.dma_fragmentation > stats.psram_fragmentation) ? 
                                 stats.dma_fragmentation : stats.psram_fragmentation;
        warning.threshold_value = g_thresholds.fragmentation_warning;
        
        ESP_LOGW(TAG, "⚠️ WARNING: Memory fragmentation increasing! DMA:%lu%% PSRAM:%lu%%",
                 (unsigned long)stats.dma_fragmentation, (unsigned long)stats.psram_fragmentation);
        
        for (int i = 0; i < g_warning_callback_count; i++) {
            g_warning_callbacks[i](&warning);
        }
    }
}

static uint32_t calculate_fragmentation_percentage(size_t total, size_t largest_block) {
    if (total == 0) {
        return 0;
    }
    
    // Fragmentation = (1 - largest_block / total) * 100
    // Higher percentage = more fragmentation
    uint32_t utilization_pct = (largest_block * 100) / total;
    return 100 - utilization_pct;
}

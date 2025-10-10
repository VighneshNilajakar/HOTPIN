/**
 * @file stt_pipeline.c
 * @brief STT audio pipeline with PSRAM ring buffer
 * 
 * Implements:
 * - 64KB PSRAM ring buffer for audio accumulation
 * - Audio capture task reading from I2S RX
 * - Streaming task sending PCM chunks to WebSocket
 * - EOS (End-of-Stream) signaling
 */

#include "stt_pipeline.h"
#include "config.h"
#include "audio_driver.h"
#include "websocket_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = TAG_STT;

// Ring buffer for audio accumulation
static uint8_t *g_audio_ring_buffer = NULL;
static size_t g_ring_buffer_size = CONFIG_STT_RING_BUFFER_SIZE;
static size_t g_ring_buffer_write_pos = 0;
static size_t g_ring_buffer_read_pos = 0;
static SemaphoreHandle_t g_ring_buffer_mutex = NULL;

// Task handles
static TaskHandle_t g_audio_capture_task_handle = NULL;
static TaskHandle_t g_audio_streaming_task_handle = NULL;

// State flags
static bool is_initialized = false;
static bool is_recording = false;
static bool is_running = false;

// Audio capture configuration
#define AUDIO_CAPTURE_CHUNK_SIZE     1024  // Bytes per read (32ms @ 16kHz, 16-bit)
#define AUDIO_STREAM_CHUNK_SIZE      4096  // Bytes per WebSocket send
#define AUDIO_CAPTURE_TIMEOUT_MS     100

// ===========================
// Private Function Declarations
// ===========================
static void audio_capture_task(void *pvParameters);
static void audio_streaming_task(void *pvParameters);
static size_t ring_buffer_available_space(void);
static size_t ring_buffer_available_data(void);
static esp_err_t ring_buffer_write(const uint8_t *data, size_t len);
static esp_err_t ring_buffer_read(uint8_t *data, size_t len, size_t *bytes_read);

// ===========================
// Public Functions
// ===========================

esp_err_t stt_pipeline_init(void) {
    ESP_LOGI(TAG, "Initializing STT pipeline...");
    
    if (is_initialized) {
        ESP_LOGW(TAG, "STT pipeline already initialized");
        return ESP_OK;
    }
    
    // CRITICAL FIX: Allocate ring buffer in external PSRAM to prevent internal DRAM exhaustion
    // The ring buffer is for software buffering AFTER DMA transfer, not for DMA operations itself.
    // Moving this 64KB buffer to PSRAM frees internal DRAM for critical I2S DMA buffers.
    ESP_LOGI(TAG, "╔════════════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "║ STT Ring Buffer Allocation (PSRAM)");
    ESP_LOGI(TAG, "╚════════════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "[MEMORY] Pre-allocation state:");
    ESP_LOGI(TAG, "  Free internal RAM: %u bytes", (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "  Free DMA-capable: %u bytes", (unsigned int)heap_caps_get_free_size(MALLOC_CAP_DMA));
    ESP_LOGI(TAG, "  Free PSRAM: %u bytes", (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    
    ESP_LOGI(TAG, "[ALLOCATION] Allocating %zu KB ring buffer in external PSRAM...", g_ring_buffer_size / 1024);
    g_audio_ring_buffer = heap_caps_malloc(g_ring_buffer_size, MALLOC_CAP_SPIRAM);
    if (g_audio_ring_buffer == NULL) {
        ESP_LOGE(TAG, "❌ CRITICAL: Failed to allocate ring buffer in PSRAM");
        ESP_LOGE(TAG, "  Requested: %zu bytes (%zu KB)", g_ring_buffer_size, g_ring_buffer_size / 1024);
        ESP_LOGE(TAG, "  Free PSRAM: %u bytes", (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        ESP_LOGE(TAG, "  This indicates PSRAM is not available or exhausted");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "  ✓ Ring buffer allocated at %p (PSRAM address)", g_audio_ring_buffer);
    
    // Verify allocation is actually in PSRAM (address range check)
    if ((uint32_t)g_audio_ring_buffer >= 0x3F800000 && (uint32_t)g_audio_ring_buffer < 0x3FC00000) {
        ESP_LOGI(TAG, "  ✓ Confirmed: Buffer is in PSRAM address range (0x3F800000-0x3FC00000)");
    } else {
        ESP_LOGW(TAG, "  ⚠ Warning: Buffer address %p may not be in expected PSRAM range", g_audio_ring_buffer);
    }
    
    ESP_LOGI(TAG, "[MEMORY] Post-allocation state:");
    ESP_LOGI(TAG, "  Free internal RAM: %u bytes", (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "  Free DMA-capable: %u bytes", (unsigned int)heap_caps_get_free_size(MALLOC_CAP_DMA));
    ESP_LOGI(TAG, "  Free PSRAM: %u bytes", (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    
    // Zero buffer safely (no memset to avoid cache issues)
    for (size_t i = 0; i < g_ring_buffer_size; i++) {
        g_audio_ring_buffer[i] = 0;
    }
    g_ring_buffer_write_pos = 0;
    g_ring_buffer_read_pos = 0;
    
    // Create ring buffer mutex
    g_ring_buffer_mutex = xSemaphoreCreateMutex();
    if (g_ring_buffer_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create ring buffer mutex");
        heap_caps_free(g_audio_ring_buffer);
        g_audio_ring_buffer = NULL;
        return ESP_ERR_NO_MEM;
    }
    
    is_initialized = true;
    ESP_LOGI(TAG, "✅ STT pipeline initialized");
    
    return ESP_OK;
}

esp_err_t stt_pipeline_deinit(void) {
    ESP_LOGI(TAG, "Deinitializing STT pipeline...");
    
    if (!is_initialized) {
        ESP_LOGW(TAG, "STT pipeline not initialized");
        return ESP_OK;
    }
    
    // Stop recording if active
    if (is_recording) {
        stt_pipeline_stop();
    }
    
    // Free resources
    if (g_ring_buffer_mutex != NULL) {
        vSemaphoreDelete(g_ring_buffer_mutex);
        g_ring_buffer_mutex = NULL;
    }
    
    if (g_audio_ring_buffer != NULL) {
        heap_caps_free(g_audio_ring_buffer);
        g_audio_ring_buffer = NULL;
    }
    
    is_initialized = false;
    ESP_LOGI(TAG, "STT pipeline deinitialized");
    
    return ESP_OK;
}

esp_err_t stt_pipeline_start(void) {
    ESP_LOGI(TAG, "Starting STT pipeline...");
    
    if (!is_initialized) {
        ESP_LOGE(TAG, "STT pipeline not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (is_running) {
        ESP_LOGW(TAG, "STT pipeline already running");
        return ESP_OK;
    }
    
    // Initialize pipeline
    if (stt_pipeline_init() != ESP_OK) {
        return ESP_FAIL;
    }
    
    // Reset ring buffer positions
    xSemaphoreTake(g_ring_buffer_mutex, portMAX_DELAY);
    g_ring_buffer_write_pos = 0;
    g_ring_buffer_read_pos = 0;
    xSemaphoreGive(g_ring_buffer_mutex);
    
    // Create audio capture task (Priority 7, Core 1)
    BaseType_t ret = xTaskCreatePinnedToCore(
        audio_capture_task,
        "stt_capture",
        TASK_STACK_SIZE_MEDIUM,
        NULL,
        TASK_PRIORITY_STT_PROCESSING,
        &g_audio_capture_task_handle,
        TASK_CORE_APP
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create audio capture task");
        return ESP_FAIL;
    }
    
    // Create audio streaming task (Priority 7, Core 1)
    ret = xTaskCreatePinnedToCore(
        audio_streaming_task,
        "stt_stream",
        TASK_STACK_SIZE_MEDIUM,
        NULL,
        TASK_PRIORITY_STT_PROCESSING,
        &g_audio_streaming_task_handle,
        TASK_CORE_APP
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create audio streaming task");
        vTaskDelete(g_audio_capture_task_handle);
        g_audio_capture_task_handle = NULL;
        return ESP_FAIL;
    }
    
    is_running = true;
    is_recording = true;
    
    ESP_LOGI(TAG, "✅ STT pipeline started");
    return ESP_OK;
}

esp_err_t stt_pipeline_stop(void) {
    ESP_LOGI(TAG, "Stopping STT pipeline...");
    
    if (!is_running) {
        ESP_LOGW(TAG, "STT pipeline not running");
        return ESP_OK;
    }
    
    is_recording = false;
    is_running = false;
    
    // Send EOS signal to server
    if (websocket_client_is_connected()) {
        ESP_LOGI(TAG, "Sending EOS signal...");
        websocket_client_send_eos();
    }
    
    // Wait for tasks to finish
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // Delete tasks
    if (g_audio_capture_task_handle != NULL) {
        vTaskDelete(g_audio_capture_task_handle);
        g_audio_capture_task_handle = NULL;
    }
    
    if (g_audio_streaming_task_handle != NULL) {
        vTaskDelete(g_audio_streaming_task_handle);
        g_audio_streaming_task_handle = NULL;
    }
    
    ESP_LOGI(TAG, "STT pipeline stopped");
    return ESP_OK;
}

bool stt_pipeline_is_recording(void) {
    return is_recording;
}

// ===========================
// Private Functions
// ===========================

static void audio_capture_task(void *pvParameters) {
    ESP_LOGI(TAG, "╔════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "║ Audio Capture Task Started on Core %d", xPortGetCoreID());
    ESP_LOGI(TAG, "╚════════════════════════════════════════════════════");
    
    // CRITICAL: Extended wait for I2S hardware to fully stabilize before first read
    ESP_LOGI(TAG, "[STABILIZATION] Phase 1: Waiting 200ms for I2S DMA...");
    ESP_LOGI(TAG, "  Current time: %lld ms", (long long)(esp_timer_get_time() / 1000));
    ESP_LOGI(TAG, "  Free heap: %u bytes", (unsigned int)esp_get_free_heap_size());
    vTaskDelay(pdMS_TO_TICKS(200));
    
    ESP_LOGI(TAG, "[STABILIZATION] Phase 2: Verify audio driver state...");
    if (!audio_driver_is_initialized()) {
        ESP_LOGE(TAG, "❌ CRITICAL: Audio driver not initialized!");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "  ✓ Audio driver initialized");
    
    ESP_LOGI(TAG, "[STABILIZATION] Phase 3: Additional 100ms settle...");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "  Total stabilization: 300ms");
    ESP_LOGI(TAG, "  Timestamp: %lld ms", (long long)(esp_timer_get_time() / 1000));
    
    ESP_LOGI(TAG, "[BUFFER] Allocating %d byte capture buffer...", AUDIO_CAPTURE_CHUNK_SIZE);
    // CRITICAL: Use DMA-capable memory (MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)
    uint8_t *capture_buffer = heap_caps_malloc(AUDIO_CAPTURE_CHUNK_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (capture_buffer == NULL) {
        ESP_LOGE(TAG, "❌ Failed to allocate DMA-capable capture buffer");
        ESP_LOGE(TAG, "  Requested: %d bytes", AUDIO_CAPTURE_CHUNK_SIZE);
        ESP_LOGE(TAG, "  Free heap: %u bytes", (unsigned int)esp_get_free_heap_size());
        ESP_LOGE(TAG, "  Free DMA-capable: %u bytes", (unsigned int)heap_caps_get_free_size(MALLOC_CAP_DMA));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "  ✓ DMA-capable buffer allocated at %p", capture_buffer);
    
    // Zero out buffer for first read
    memset(capture_buffer, 0, AUDIO_CAPTURE_CHUNK_SIZE);
    
    size_t bytes_read = 0;
    uint32_t total_bytes_captured = 0;
    uint32_t read_count = 0;
    uint32_t error_count = 0;
    int64_t first_read_time = 0;
    
    ESP_LOGI(TAG, "╔════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "║ 🎤 STARTING AUDIO CAPTURE");
    ESP_LOGI(TAG, "║ Chunk size: %d bytes | Timeout: %d ms", AUDIO_CAPTURE_CHUNK_SIZE, AUDIO_CAPTURE_TIMEOUT_MS);
    ESP_LOGI(TAG, "╚════════════════════════════════════════════════════");
    
    while (is_running) {
        int64_t read_start = esp_timer_get_time();
        
        // Read audio from I2S RX (microphone)
        esp_err_t ret = audio_driver_read(capture_buffer, AUDIO_CAPTURE_CHUNK_SIZE, 
                                           &bytes_read, AUDIO_CAPTURE_TIMEOUT_MS);
        
        int64_t read_duration = (esp_timer_get_time() - read_start) / 1000;
        read_count++;
        
        // Log first read with detailed diagnostics
        if (read_count == 1) {
            first_read_time = esp_timer_get_time() / 1000;
            ESP_LOGI(TAG, "[FIRST READ] Completed:");
            ESP_LOGI(TAG, "  Result: %s", esp_err_to_name(ret));
            ESP_LOGI(TAG, "  Bytes read: %zu / %d", bytes_read, AUDIO_CAPTURE_CHUNK_SIZE);
            ESP_LOGI(TAG, "  Duration: %lld ms", (long long)read_duration);
            ESP_LOGI(TAG, "  Timestamp: %lld ms", (long long)first_read_time);
            
            // CRITICAL: Small delay for cache coherency after DMA transfer
            if (bytes_read >= 16) {
                vTaskDelay(pdMS_TO_TICKS(1)); // 1ms to ensure DMA completion
                ESP_LOGI(TAG, "  First 16 bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                         capture_buffer[0], capture_buffer[1], capture_buffer[2], capture_buffer[3],
                         capture_buffer[4], capture_buffer[5], capture_buffer[6], capture_buffer[7],
                         capture_buffer[8], capture_buffer[9], capture_buffer[10], capture_buffer[11],
                         capture_buffer[12], capture_buffer[13], capture_buffer[14], capture_buffer[15]);
            }
        }
        
        if (ret == ESP_OK && bytes_read > 0) {
            // Write to ring buffer
            ret = ring_buffer_write(capture_buffer, bytes_read);
            if (ret == ESP_OK) {
                total_bytes_captured += bytes_read;
                
                // Log every 10th successful read
                if (read_count % 10 == 0) {
                    ESP_LOGI(TAG, "[CAPTURE] Read #%u: %zu bytes (total: %u bytes, %.1f KB)",
                             (unsigned int)read_count, bytes_read, (unsigned int)total_bytes_captured, total_bytes_captured / 1024.0);
                    ESP_LOGI(TAG, "  Avg read time: %lld ms | Errors: %u", (long long)read_duration, (unsigned int)error_count);
                }
            } else {
                ESP_LOGW(TAG, "⚠ Ring buffer full - dropping %zu bytes (read #%u)", bytes_read, (unsigned int)read_count);
            }
        } else if (ret != ESP_OK) {
            error_count++;
            ESP_LOGE(TAG, "❌ I2S read error #%u (read #%u): %s", (unsigned int)error_count, (unsigned int)read_count, esp_err_to_name(ret));
            ESP_LOGE(TAG, "  Bytes read: %zu | Duration: %lld ms", bytes_read, (long long)read_duration);
            ESP_LOGE(TAG, "  Free heap: %u bytes", (unsigned int)esp_get_free_heap_size());
            
            // If first few reads fail, something is seriously wrong
            if (read_count < 5) {
                ESP_LOGE(TAG, "❌ CRITICAL: Early read failure - I2S may not be properly initialized");
            }
            
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    
    ESP_LOGI(TAG, "Audio capture task stopped (captured %u bytes total)", (unsigned int)total_bytes_captured);
    heap_caps_free(capture_buffer);
    vTaskDelete(NULL);
}

static void audio_streaming_task(void *pvParameters) {
    ESP_LOGI(TAG, "Audio streaming task started on Core %d", xPortGetCoreID());
    
    uint8_t *stream_buffer = heap_caps_malloc(AUDIO_STREAM_CHUNK_SIZE, MALLOC_CAP_INTERNAL);
    if (stream_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate stream buffer");
        vTaskDelete(NULL);
        return;
    }
    
    size_t bytes_read = 0;
    uint32_t total_bytes_streamed = 0;
    uint32_t chunk_count = 0;
    
    // Wait for WebSocket connection
    while (is_running && !websocket_client_is_connected()) {
        ESP_LOGW(TAG, "Waiting for WebSocket connection...");
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    
    if (!websocket_client_is_connected()) {
        ESP_LOGE(TAG, "WebSocket not connected - aborting streaming");
        heap_caps_free(stream_buffer);
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "Starting audio streaming to server...");
    
    while (is_running) {
        // Check if enough data is available
        size_t available = ring_buffer_available_data();
        
        if (available >= AUDIO_STREAM_CHUNK_SIZE) {
            // Read chunk from ring buffer
            esp_err_t ret = ring_buffer_read(stream_buffer, AUDIO_STREAM_CHUNK_SIZE, &bytes_read);
            
            if (ret == ESP_OK && bytes_read > 0) {
                // Send to WebSocket server
                ret = websocket_client_send_audio(stream_buffer, bytes_read);
                
                if (ret == ESP_OK) {
                    total_bytes_streamed += bytes_read;
                    chunk_count++;
                    ESP_LOGD(TAG, "Streamed chunk #%u (%zu bytes, total: %u)", 
                             (unsigned int)chunk_count, bytes_read, (unsigned int)total_bytes_streamed);
                } else {
                    ESP_LOGE(TAG, "WebSocket send failed: %s", esp_err_to_name(ret));
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
            }
        } else {
            // Not enough data - wait a bit
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    
    ESP_LOGI(TAG, "Audio streaming task stopped (streamed %u bytes in %u chunks)", 
             (unsigned int)total_bytes_streamed, (unsigned int)chunk_count);
    heap_caps_free(stream_buffer);
    vTaskDelete(NULL);
}

// Ring buffer helper functions
static size_t ring_buffer_available_space(void) {
    size_t space;
    xSemaphoreTake(g_ring_buffer_mutex, portMAX_DELAY);
    
    if (g_ring_buffer_write_pos >= g_ring_buffer_read_pos) {
        space = g_ring_buffer_size - (g_ring_buffer_write_pos - g_ring_buffer_read_pos);
    } else {
        space = g_ring_buffer_read_pos - g_ring_buffer_write_pos;
    }
    
    xSemaphoreGive(g_ring_buffer_mutex);
    return space;
}

static size_t ring_buffer_available_data(void) {
    size_t data;
    xSemaphoreTake(g_ring_buffer_mutex, portMAX_DELAY);
    
    if (g_ring_buffer_write_pos >= g_ring_buffer_read_pos) {
        data = g_ring_buffer_write_pos - g_ring_buffer_read_pos;
    } else {
        data = g_ring_buffer_size - (g_ring_buffer_read_pos - g_ring_buffer_write_pos);
    }
    
    xSemaphoreGive(g_ring_buffer_mutex);
    return data;
}

static esp_err_t ring_buffer_write(const uint8_t *data, size_t len) {
    // Validate inputs
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (ring_buffer_available_space() < len) {
        return ESP_ERR_NO_MEM;  // Buffer full
    }
    
    if (!xSemaphoreTake(g_ring_buffer_mutex, pdMS_TO_TICKS(100))) {
        ESP_LOGW(TAG, "⚠ Ring buffer mutex timeout");
        return ESP_ERR_TIMEOUT;
    }
    
    // Safe byte-by-byte copy with bounds checking
    for (size_t i = 0; i < len; i++) {
        // Double-check we're within bounds (paranoid safety)
        if (g_ring_buffer_write_pos >= g_ring_buffer_size) {
            ESP_LOGE(TAG, "❌ Ring buffer write pos overflow: %zu >= %zu", 
                     g_ring_buffer_write_pos, g_ring_buffer_size);
            xSemaphoreGive(g_ring_buffer_mutex);
            return ESP_ERR_INVALID_STATE;
        }
        
        g_audio_ring_buffer[g_ring_buffer_write_pos] = data[i];
        g_ring_buffer_write_pos = (g_ring_buffer_write_pos + 1) % g_ring_buffer_size;
    }
    
    xSemaphoreGive(g_ring_buffer_mutex);
    return ESP_OK;
}

static esp_err_t ring_buffer_read(uint8_t *data, size_t len, size_t *bytes_read) {
    // Validate inputs
    if (data == NULL || bytes_read == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    size_t available = ring_buffer_available_data();
    size_t to_read = (len < available) ? len : available;
    
    if (to_read == 0) {
        *bytes_read = 0;
        return ESP_OK;
    }
    
    if (!xSemaphoreTake(g_ring_buffer_mutex, pdMS_TO_TICKS(100))) {
        ESP_LOGW(TAG, "⚠ Ring buffer mutex timeout (read)");
        *bytes_read = 0;
        return ESP_ERR_TIMEOUT;
    }
    
    // Safe byte-by-byte copy with bounds checking
    for (size_t i = 0; i < to_read; i++) {
        // Double-check we're within bounds (paranoid safety)
        if (g_ring_buffer_read_pos >= g_ring_buffer_size) {
            ESP_LOGE(TAG, "❌ Ring buffer read pos overflow: %zu >= %zu", 
                     g_ring_buffer_read_pos, g_ring_buffer_size);
            xSemaphoreGive(g_ring_buffer_mutex);
            *bytes_read = i;  // Return what we managed to read
            return ESP_ERR_INVALID_STATE;
        }
        
        data[i] = g_audio_ring_buffer[g_ring_buffer_read_pos];
        g_ring_buffer_read_pos = (g_ring_buffer_read_pos + 1) % g_ring_buffer_size;
    }
    
    xSemaphoreGive(g_ring_buffer_mutex);
    
    *bytes_read = to_read;
    return ESP_OK;
}

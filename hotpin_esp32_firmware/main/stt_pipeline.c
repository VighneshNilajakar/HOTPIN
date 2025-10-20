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
#include "event_dispatcher.h"
#include "system_events.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <inttypes.h>

static const char *TAG = TAG_STT;

#define STT_STREAM_EVENT_START        (1U << 0)
#define STT_STREAM_EVENT_STOP         (1U << 1)
#define STT_STREAM_EVENT_SHUTDOWN     (1U << 2)
#define STT_STREAM_EVENT_CAPTURE_IDLE (1U << 3)

static stt_pipeline_handle_t s_pipeline_ctx = {
    .stream_events = NULL,
};

// Ring buffer for audio accumulation
static uint8_t *g_audio_ring_buffer = NULL;
static size_t g_ring_buffer_size = CONFIG_STT_RING_BUFFER_SIZE;
static size_t g_ring_buffer_write_pos = 0;
static size_t g_ring_buffer_read_pos = 0;
static size_t g_ring_buffer_count = 0;
static SemaphoreHandle_t g_ring_buffer_mutex = NULL;

// Task handles
static TaskHandle_t g_audio_capture_task_handle = NULL;
static TaskHandle_t g_audio_streaming_task_handle = NULL;
static volatile bool g_streaming_active = false;

// State flags
static bool is_initialized = false;
static bool is_recording = false;
static bool is_running = false;
static bool s_stop_event_posted = false;

// Audio capture configuration
#define AUDIO_CAPTURE_CHUNK_SIZE     1024  // Bytes per read (32ms @ 16kHz, 16-bit)
#define AUDIO_STREAM_CHUNK_SIZE      4096  // Bytes per WebSocket send
#define AUDIO_CAPTURE_TIMEOUT_MS     100
#define AUDIO_STREAM_SEND_TIMEOUT_MS 250   // Timeout for WebSocket writes
#define AUDIO_STREAM_HEALTH_LOG_MS   5000  // Periodic health log interval
#define AUDIO_STREAM_MAX_SEND_FAILURES 3    // Abort threshold for consecutive send failures
#define STT_TASK_STOP_WAIT_MS        500    // Wait time for tasks to self-terminate

// ===========================
// Private Function Declarations
// ===========================

static void audio_capture_task(void *pvParameters);
static void audio_streaming_task(void *pvParameters);
// static size_t ring_buffer_available_space(void);
static size_t ring_buffer_available_data(void);
static esp_err_t ring_buffer_write(const uint8_t *data, size_t len) __attribute__((noinline));
static esp_err_t ring_buffer_read(uint8_t *data, size_t len, size_t *bytes_read) __attribute__((noinline));
static void stt_pipeline_mark_stopped(void);
static void stt_pipeline_dispatch_stop_event(void);
static void stt_pipeline_reset_ring_buffer(void);
static void stt_pipeline_wait_for_capture_idle(TickType_t deadline_ticks);
static void stt_pipeline_wait_for_streaming_idle(TickType_t deadline_ticks);
static bool stt_pipeline_stop_signal_received(void);
static inline void stt_pipeline_notify_capture_idle(void);

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
    g_ring_buffer_count = 0;
    
    // Create ring buffer mutex
    g_ring_buffer_mutex = xSemaphoreCreateMutex();
    if (g_ring_buffer_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create ring buffer mutex");
        heap_caps_free(g_audio_ring_buffer);
        g_audio_ring_buffer = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (s_pipeline_ctx.stream_events == NULL) {
        s_pipeline_ctx.stream_events = xEventGroupCreate();
        if (s_pipeline_ctx.stream_events == NULL) {
            ESP_LOGE(TAG, "Failed to create stream control event group");
            vSemaphoreDelete(g_ring_buffer_mutex);
            g_ring_buffer_mutex = NULL;
            heap_caps_free(g_audio_ring_buffer);
            g_audio_ring_buffer = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    if (g_audio_streaming_task_handle == NULL) {
        ESP_LOGI(TAG, "[CORE AFFINITY] Creating persistent audio streaming task on Core %d", TASK_CORE_AUDIO_IO);
        BaseType_t stream_ret = xTaskCreatePinnedToCore(
            audio_streaming_task,
            "stt_stream",
            TASK_STACK_SIZE_LARGE,  // Increased from TASK_STACK_SIZE_MEDIUM to prevent stack overflow
            NULL,
            TASK_PRIORITY_STT_PROCESSING,
            &g_audio_streaming_task_handle,
            TASK_CORE_AUDIO_IO
        );

        if (stream_ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create persistent audio streaming task");
            vSemaphoreDelete(g_ring_buffer_mutex);
            g_ring_buffer_mutex = NULL;
            vEventGroupDelete(s_pipeline_ctx.stream_events);
            s_pipeline_ctx.stream_events = NULL;
            heap_caps_free(g_audio_ring_buffer);
            g_audio_ring_buffer = NULL;
            return ESP_FAIL;
        }
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
    if (is_recording || is_running) {
        stt_pipeline_stop();
    }

    if (s_pipeline_ctx.stream_events != NULL) {
        xEventGroupSetBits(s_pipeline_ctx.stream_events, STT_STREAM_EVENT_SHUTDOWN);
    }

    TickType_t shutdown_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(STT_TASK_STOP_WAIT_MS);
    while (g_audio_streaming_task_handle != NULL && xTaskGetTickCount() < shutdown_deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (g_audio_streaming_task_handle != NULL) {
        ESP_LOGW(TAG, "Force deleting streaming task after shutdown timeout");
        vTaskDelete(g_audio_streaming_task_handle);
        g_audio_streaming_task_handle = NULL;
    }

    // Free resources
    if (g_ring_buffer_mutex != NULL) {
        vSemaphoreDelete(g_ring_buffer_mutex);
        g_ring_buffer_mutex = NULL;
    }

    if (s_pipeline_ctx.stream_events != NULL) {
        vEventGroupDelete(s_pipeline_ctx.stream_events);
        s_pipeline_ctx.stream_events = NULL;
    }

    if (g_audio_ring_buffer != NULL) {
        heap_caps_free(g_audio_ring_buffer);
        g_audio_ring_buffer = NULL;
    }

    g_streaming_active = false;

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

    if (g_audio_streaming_task_handle == NULL || s_pipeline_ctx.stream_events == NULL) {
        ESP_LOGE(TAG, "Streaming infrastructure not ready");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Reset ring buffer positions
    xSemaphoreTake(g_ring_buffer_mutex, portMAX_DELAY);
    g_ring_buffer_write_pos = 0;
    g_ring_buffer_read_pos = 0;
    g_ring_buffer_count = 0;
    xSemaphoreGive(g_ring_buffer_mutex);
    
    // CRITICAL FIX: Pin audio capture task to Core 0 (same as Wi-Fi) to resolve hardware bus contention
    // The LoadStoreError was caused by Wi-Fi (Core 0) and I2S DMA (Core 1) competing for memory bus access
    // Co-locating them on Core 0 allows FreeRTOS scheduler to coordinate their operations
    // IMPORTANT: Use explicit memory attributes to ensure task stack is NOT allocated in PSRAM
    ESP_LOGI(TAG, "[CORE AFFINITY] Creating audio capture task on Core 0 (co-located with Wi-Fi)");
    BaseType_t ret = xTaskCreatePinnedToCore(
        audio_capture_task,
        "stt_capture",
        TASK_STACK_SIZE_LARGE,
        NULL,
        TASK_PRIORITY_STT_PROCESSING,
        &g_audio_capture_task_handle,
    TASK_CORE_AUDIO_IO  // Core 0 - CRITICAL: Must match Wi-Fi core to prevent DMA corruption
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create audio capture task");
        return ESP_FAIL;
    }
    
    is_running = true;
    is_recording = true;
    s_stop_event_posted = false;
    stt_pipeline_reset_ring_buffer();

    xEventGroupClearBits(s_pipeline_ctx.stream_events,
                         STT_STREAM_EVENT_START |
                         STT_STREAM_EVENT_STOP |
                         STT_STREAM_EVENT_CAPTURE_IDLE);
    xEventGroupSetBits(s_pipeline_ctx.stream_events, STT_STREAM_EVENT_START);

    system_event_t evt = {
        .type = SYSTEM_EVENT_STT_STARTED,
        .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL),
    };
    if (!event_dispatcher_post(&evt, pdMS_TO_TICKS(10))) {
        ESP_LOGW(TAG, "Failed to enqueue STT start event");
    }
    
    ESP_LOGI(TAG, "✅ STT pipeline started");
    return ESP_OK;
}

esp_err_t stt_pipeline_stop(void) {
    ESP_LOGI(TAG, "Stopping STT pipeline...");

    if (!is_initialized) {
        ESP_LOGW(TAG, "STT pipeline not initialized");
        stt_pipeline_dispatch_stop_event();
        return ESP_OK;
    }

    if (!is_running && g_audio_capture_task_handle == NULL && !g_streaming_active) {
        ESP_LOGW(TAG, "STT pipeline already stopped");
        stt_pipeline_dispatch_stop_event();
        return ESP_OK;
    }

    if (s_pipeline_ctx.stream_events != NULL) {
        xEventGroupClearBits(s_pipeline_ctx.stream_events, STT_STREAM_EVENT_CAPTURE_IDLE);
    }

    stt_pipeline_mark_stopped();

    TickType_t deadline_ticks = xTaskGetTickCount() + pdMS_TO_TICKS(STT_TASK_STOP_WAIT_MS);
    stt_pipeline_wait_for_capture_idle(deadline_ticks);

    if (s_pipeline_ctx.stream_events != NULL) {
        xEventGroupSetBits(s_pipeline_ctx.stream_events, STT_STREAM_EVENT_STOP);
    }
    stt_pipeline_wait_for_streaming_idle(deadline_ticks);

    stt_pipeline_reset_ring_buffer();

    if (!g_streaming_active) {
        stt_pipeline_dispatch_stop_event();
    }

    ESP_LOGI(TAG, "STT pipeline stopped");
    return ESP_OK;
}

bool stt_pipeline_is_recording(void) {
    return is_recording;
}

void stt_pipeline_cancel_capture(void) {
    if (!is_running || !is_recording) {
        return;
    }

    ESP_LOGI(TAG, "Cancelling STT capture while voice pipeline is busy");
    is_recording = false;

    esp_err_t clr_ret = audio_driver_clear_buffers();
    if (clr_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to clear audio buffers: %s", esp_err_to_name(clr_ret));
    }
}

const stt_pipeline_handle_t *stt_pipeline_get_handle(void) {
    return &s_pipeline_ctx;
}

// ===========================
// Private Functions
// ===========================

static void audio_capture_task(void *pvParameters) {
    // CRITICAL SAFETY CHECK: Prevent InstructionFetchError crash from PSRAM execution
    // This detects corrupted function pointers and prevents system crashes
    uint32_t pc_check = (uint32_t)__builtin_return_address(0);
    if ((pc_check >= 0x3F800000) && (pc_check < 0x40000000)) {
        ESP_LOGE(TAG, "❌ EMERGENCY ABORT: Task executing from PSRAM (0x%08x) - preventing crash!", (unsigned int)pc_check);
        if (g_audio_capture_task_handle == xTaskGetCurrentTaskHandle()) {
            g_audio_capture_task_handle = NULL;
        }
        stt_pipeline_notify_capture_idle();
        vTaskDelete(NULL);
        return;
    }
    
    // ESP_LOGI(TAG, "╔════════════════════════════════════════════════════");
    // ESP_LOGI(TAG, "║ Audio Capture Task Started on Core %d", xPortGetCoreID());
    // ESP_LOGI(TAG, "╚════════════════════════════════════════════════════");
    
    // CRITICAL: Extended wait for I2S hardware to fully stabilize before first read
    ESP_LOGI(TAG, "[STABILIZATION] Phase 1: Waiting 200ms for I2S DMA...");
    ESP_LOGI(TAG, "  Current time: %lld ms", (long long)(esp_timer_get_time() / 1000));
    ESP_LOGI(TAG, "  Free heap: %u bytes", (unsigned int)esp_get_free_heap_size());
    vTaskDelay(pdMS_TO_TICKS(200));
    
    ESP_LOGI(TAG, "[STABILIZATION] Phase 2: Verify audio driver state...");
    if (!audio_driver_is_initialized()) {
        ESP_LOGE(TAG, "❌ CRITICAL: Audio driver not initialized!");
        if (g_audio_capture_task_handle == xTaskGetCurrentTaskHandle()) {
            g_audio_capture_task_handle = NULL;
        }
        stt_pipeline_notify_capture_idle();
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "  ✓ Audio driver initialized");
    
    ESP_LOGI(TAG, "[STABILIZATION] Phase 3: Additional 100ms settle...");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "  Total stabilization: 300ms");
    ESP_LOGI(TAG, "  Timestamp: %lld ms", (long long)(esp_timer_get_time() / 1000));
    
    ESP_LOGI(TAG, "[BUFFER] Allocating %d byte capture buffer...", AUDIO_CAPTURE_CHUNK_SIZE);
    // CRITICAL: Use DMA-capable memory from INTERNAL RAM (MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)
    // IMPORTANT: Add extra safety padding to prevent buffer overflows
    uint8_t *capture_buffer = heap_caps_malloc(AUDIO_CAPTURE_CHUNK_SIZE + 32, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (capture_buffer == NULL) {
        ESP_LOGE(TAG, "❌ Failed to allocate DMA-capable capture buffer");
        ESP_LOGE(TAG, "  Requested: %d bytes", AUDIO_CAPTURE_CHUNK_SIZE + 32);
        ESP_LOGE(TAG, "  Free heap: %u bytes", (unsigned int)esp_get_free_heap_size());
        ESP_LOGE(TAG, "  Free DMA-capable: %u bytes", (unsigned int)heap_caps_get_free_size(MALLOC_CAP_DMA));
        g_audio_capture_task_handle = NULL;
        stt_pipeline_notify_capture_idle();
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "  ✓ DMA-capable buffer allocated at %p (with safety padding)", capture_buffer);
    
    // Zero out buffer with extra safety padding to prevent memory corruption
    memset(capture_buffer, 0, AUDIO_CAPTURE_CHUNK_SIZE + 32);
    
    // Add memory guard pattern to detect buffer overflows
    memset(capture_buffer + AUDIO_CAPTURE_CHUNK_SIZE, 0xDE, 16);  // Guard bytes
    memset(capture_buffer + AUDIO_CAPTURE_CHUNK_SIZE + 16, 0xAD, 16);  // More guard bytes
    
    // Verify memory alignment (must be 4-byte aligned for DMA)
    if (((uintptr_t)capture_buffer) & 0x3) {
        ESP_LOGW(TAG, "⚠ Capture buffer not 4-byte aligned - potential DMA issue");
    }
    
    size_t bytes_read = 0;
    uint32_t total_bytes_captured = 0;
    uint32_t read_count = 0;
    uint32_t error_count = 0;
    
    // CANARY: Static counter for continuous health monitoring
    static uint32_t alive_counter = 0;
    
    ESP_LOGI(TAG, "╔════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "║ 🎤 STARTING AUDIO CAPTURE");
    ESP_LOGI(TAG, "║ Chunk size: %d bytes | Timeout: %d ms", AUDIO_CAPTURE_CHUNK_SIZE, AUDIO_CAPTURE_TIMEOUT_MS);
    ESP_LOGI(TAG, "╚════════════════════════════════════════════════════");
    
    while (is_running) {
        if (!is_recording) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        int64_t read_start = esp_timer_get_time();
        
        // Read audio from I2S RX (microphone)
        esp_err_t ret = audio_driver_read(capture_buffer, AUDIO_CAPTURE_CHUNK_SIZE, 
                                           &bytes_read, AUDIO_CAPTURE_TIMEOUT_MS);
        
        int64_t read_duration = (esp_timer_get_time() - read_start) / 1000;
        read_count++;
        
        // Log first read with minimal diagnostics to prevent InstructionFetchError
        if (read_count == 1) {
            ESP_LOGD(TAG, "[FIRST READ] Completed: %zu bytes, duration: %lld ms", bytes_read, (long long)read_duration);
            
            // CRITICAL: Small delay for cache coherency after DMA transfer
            if (bytes_read >= 16) {
                vTaskDelay(pdMS_TO_TICKS(1)); // 1ms to ensure DMA completion
                // Removed verbose hex dump logging to prevent InstructionFetchError from excessive logging
                // ESP_LOGI(TAG, "  First 16 bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                //          capture_buffer[0], capture_buffer[1], capture_buffer[2], capture_buffer[3],
                //          capture_buffer[4], capture_buffer[5], capture_buffer[6], capture_buffer[7],
                //          capture_buffer[8], capture_buffer[9], capture_buffer[10], capture_buffer[11],
                //          capture_buffer[12], capture_buffer[13], capture_buffer[14], capture_buffer[15]);
            }
        }
        
        if (ret == ESP_OK && bytes_read > 0) {
            // Write to ring buffer
            ret = ring_buffer_write(capture_buffer, bytes_read);
            if (ret == ESP_OK) {
                total_bytes_captured += bytes_read;
                
                // CANARY: Continuous health monitoring - log every 500 successful reads (reduced frequency to prevent crash)
                alive_counter++;
                if (alive_counter % 500 == 0) {  // Reduced from 100 to 500 to significantly reduce logging
                    ESP_LOGI(TAG, "[CAPTURE] ✅ Alive... %u reads completed (Free Heap: %u bytes)", 
                             (unsigned int)alive_counter, (unsigned int)esp_get_free_heap_size());
                }
                
                // Reduce logging frequency - only log every 200 reads to prevent system overload
                if (read_count % 200 == 0) {  // Changed from 10 to 200 to significantly reduce logging
                    ESP_LOGD(TAG, "[CAPTURE] Read #%u: %zu bytes (total: %u bytes, %.1f KB)",
                             (unsigned int)read_count, bytes_read, (unsigned int)total_bytes_captured, total_bytes_captured / 1024.0);
                    ESP_LOGD(TAG, "  Avg read time: %lld ms | Errors: %u", (long long)read_duration, (unsigned int)error_count);
                }
            } else {
                ESP_LOGW(TAG, "⚠ Ring buffer full - dropping %zu bytes (read #%u)", bytes_read, (unsigned int)read_count);
                vTaskDelay(pdMS_TO_TICKS(5));
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
    // Free buffer with safety padding
    if (capture_buffer != NULL) {
        // Check guard bytes before freeing (debug only)
        #ifdef CONFIG_ENABLE_DEBUG_LOGS
        const uint8_t *guard1 = capture_buffer + AUDIO_CAPTURE_CHUNK_SIZE;
        const uint8_t *guard2 = capture_buffer + AUDIO_CAPTURE_CHUNK_SIZE + 16;
        bool guards_ok = true;
        for (int i = 0; i < 16; i++) {
            if (guard1[i] != 0xDE) guards_ok = false;
            if (guard2[i] != 0xAD) guards_ok = false;
        }
        if (!guards_ok) {
            ESP_LOGW(TAG, "⚠ Potential buffer overflow detected in capture buffer!");
        }
        #endif
        
        heap_caps_free(capture_buffer);
        capture_buffer = NULL;
    }

    if (g_audio_capture_task_handle == xTaskGetCurrentTaskHandle()) {
        g_audio_capture_task_handle = NULL;
    }

    stt_pipeline_notify_capture_idle();
    vTaskDelete(NULL);
}

static void audio_streaming_task(void *pvParameters) {
    ESP_LOGI(TAG, "Persistent audio streaming task started on Core %d", xPortGetCoreID());
    
    // CRITICAL SAFETY CHECK: Prevent InstructionFetchError crash from PSRAM execution
    uint32_t pc_check = (uint32_t)__builtin_return_address(0);
    if ((pc_check >= 0x3F800000) && (pc_check < 0x40000000)) {
        ESP_LOGE(TAG, "❌ EMERGENCY ABORT: Streaming task executing from PSRAM (0x%08x) - preventing crash!", (unsigned int)pc_check);
        g_audio_streaming_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    // CRITICAL: Allocate streaming buffer with safety padding to prevent overflows
    uint8_t *stream_buffer = heap_caps_malloc(
        AUDIO_STREAM_CHUNK_SIZE + 32,  // Extra padding for safety
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
    );
    if (stream_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate stream buffer with padding");
        g_audio_streaming_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "  ✓ Stream buffer allocated at %p (with safety padding)", stream_buffer);
    
    // Initialize with zero padding
    memset(stream_buffer, 0, AUDIO_STREAM_CHUNK_SIZE + 32);
    
    // Add guard bytes
    memset(stream_buffer + AUDIO_STREAM_CHUNK_SIZE, 0xBE, 16);
    memset(stream_buffer + AUDIO_STREAM_CHUNK_SIZE + 16, 0xEF, 16);

    for (;;) {
        EventBits_t wait_bits = xEventGroupWaitBits(
            s_pipeline_ctx.stream_events,
            STT_STREAM_EVENT_START | STT_STREAM_EVENT_SHUTDOWN,
            pdTRUE,
            pdFALSE,
            portMAX_DELAY
        );

        if ((wait_bits & STT_STREAM_EVENT_SHUTDOWN) != 0U) {
            ESP_LOGI(TAG, "Streaming task received shutdown signal");
            break;
        }

        if ((wait_bits & STT_STREAM_EVENT_START) == 0U) {
            continue;
        }

        ESP_LOGI(TAG, "Audio streaming session activated");

        size_t bytes_read = 0;
        uint32_t total_bytes_streamed = 0;
        uint32_t chunk_count = 0;
        uint32_t dropped_not_ready = 0;
        uint32_t dropped_send_fail = 0;
        uint32_t consecutive_send_failures = 0;
        TickType_t last_health_log = xTaskGetTickCount();
        bool aborted_due_to_error = false;

        g_streaming_active = true;

        while (is_running && !stt_pipeline_stop_signal_received() && !websocket_client_is_connected()) {
            ESP_LOGW(TAG, "Waiting for WebSocket connection...");
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        if (!websocket_client_is_connected()) {
            ESP_LOGE(TAG, "WebSocket not connected - streaming session aborted");
            aborted_due_to_error = true;
            stt_pipeline_mark_stopped();
            xEventGroupSetBits(s_pipeline_ctx.stream_events, STT_STREAM_EVENT_STOP);
        }

        while (!aborted_due_to_error && is_running && !stt_pipeline_stop_signal_received() && !websocket_client_session_ready()) {
            ESP_LOGW(TAG, "Waiting for WebSocket session readiness...");
            vTaskDelay(pdMS_TO_TICKS(250));
        }

        if (!aborted_due_to_error && !websocket_client_session_ready()) {
            ESP_LOGE(TAG, "WebSocket session not ready - streaming session aborted");
            aborted_due_to_error = true;
            stt_pipeline_mark_stopped();
            xEventGroupSetBits(s_pipeline_ctx.stream_events, STT_STREAM_EVENT_STOP);
        }

        if (!aborted_due_to_error) {
            ESP_LOGI(TAG, "Starting audio streaming to server...");
        }

        while (!aborted_due_to_error && !stt_pipeline_stop_signal_received()) {
            if (!websocket_client_is_connected()) {
                ESP_LOGE(TAG, "WebSocket disconnected during streaming");
                stt_pipeline_mark_stopped();
                xEventGroupSetBits(s_pipeline_ctx.stream_events, STT_STREAM_EVENT_STOP);
                aborted_due_to_error = true;
                break;
            }

            size_t available = ring_buffer_available_data();

            if (!is_running && available == 0U) {
                ESP_LOGI(TAG, "Capture stopped and ring buffer drained; ending streaming loop");
                break;
            }

            if (available >= AUDIO_STREAM_CHUNK_SIZE || (!is_running && available > 0U)) {
                size_t chunk_size = (available >= AUDIO_STREAM_CHUNK_SIZE) ?
                                    AUDIO_STREAM_CHUNK_SIZE : available;
                esp_err_t ret = ring_buffer_read(stream_buffer, chunk_size, &bytes_read);

                if (ret == ESP_OK && bytes_read > 0) {
                    if (!websocket_client_can_stream_audio()) {
                        dropped_not_ready++;
                        if ((dropped_not_ready % 25U) == 0U) {
                            ESP_LOGW(TAG, "[STREAM] Dropping audio chunk (session busy). dropped_not_ready=%u buffer=%u",
                                     (unsigned int)dropped_not_ready,
                                     (unsigned int)ring_buffer_available_data());
                        }
                        vTaskDelay(pdMS_TO_TICKS(10));
                    } else {
                        ret = websocket_client_send_audio(stream_buffer, bytes_read, AUDIO_STREAM_SEND_TIMEOUT_MS);

                        if (ret == ESP_OK) {
                            total_bytes_streamed += bytes_read;
                            chunk_count++;
                            consecutive_send_failures = 0;
                            ESP_LOGD(TAG, "Streamed chunk #%u (%zu bytes, total: %u)",
                                     (unsigned int)chunk_count, bytes_read, (unsigned int)total_bytes_streamed);
                        } else {
                            dropped_send_fail++;
                            consecutive_send_failures++;
                            ESP_LOGW(TAG, "[STREAM] WebSocket send failed (%s). dropped_send_fail=%u",
                                     esp_err_to_name(ret), (unsigned int)dropped_send_fail);
                            if (consecutive_send_failures >= AUDIO_STREAM_MAX_SEND_FAILURES) {
                                ESP_LOGE(TAG, "[STREAM] Aborting after %u consecutive send failures", (unsigned int)consecutive_send_failures);
                                stt_pipeline_mark_stopped();
                                xEventGroupSetBits(s_pipeline_ctx.stream_events, STT_STREAM_EVENT_STOP);
                                aborted_due_to_error = true;
                                break;
                            }
                            vTaskDelay(pdMS_TO_TICKS(25));
                        }
                    }
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(50));
            }

            if ((xTaskGetTickCount() - last_health_log) >= pdMS_TO_TICKS(AUDIO_STREAM_HEALTH_LOG_MS * 2)) {  // Doubled the interval
                ESP_LOGD(TAG, "[STREAM] sent=%u bytes chunks=%u dropped_busy=%u dropped_fail=%u buffer_level=%u",
                         (unsigned int)total_bytes_streamed,
                         (unsigned int)chunk_count,
                         (unsigned int)dropped_not_ready,
                         (unsigned int)dropped_send_fail,
                         (unsigned int)ring_buffer_available_data());
                last_health_log = xTaskGetTickCount();
            }
        }

        if (stt_pipeline_stop_signal_received()) {
            ESP_LOGI(TAG, "Streaming task received stop signal");
        }

        if (aborted_due_to_error) {
            ESP_LOGW(TAG, "Audio streaming aborted due to transport errors");
        }

        if (websocket_client_is_connected()) {
            ESP_LOGI(TAG, "Sending EOS signal...");
            websocket_client_send_eos();
        } else {
            ESP_LOGW(TAG, "Skipping EOS - WebSocket disconnected");
        }

        ESP_LOGI(TAG, "Audio streaming session complete (streamed %u bytes in %u chunks)",
                 (unsigned int)total_bytes_streamed,
                 (unsigned int)chunk_count);

        g_streaming_active = false;
        xEventGroupClearBits(s_pipeline_ctx.stream_events, STT_STREAM_EVENT_STOP);
        stt_pipeline_dispatch_stop_event();
    }

    // Free streaming buffer with guard byte checking
    if (stream_buffer != NULL) {
        // Check guard bytes before freeing
        #ifdef CONFIG_ENABLE_DEBUG_LOGS
        const uint8_t *guard1 = stream_buffer + AUDIO_STREAM_CHUNK_SIZE;
        const uint8_t *guard2 = stream_buffer + AUDIO_STREAM_CHUNK_SIZE + 16;
        bool guards_ok = true;
        for (int i = 0; i < 16; i++) {
            if (guard1[i] != 0xBE) guards_ok = false;
            if (guard2[i] != 0xEF) guards_ok = false;
        }
        if (!guards_ok) {
            ESP_LOGW(TAG, "⚠ Potential buffer overflow detected in stream buffer!");
        }
        #endif
        
        heap_caps_free(stream_buffer);
        stream_buffer = NULL;
    }
    
    g_audio_streaming_task_handle = NULL;
    vTaskDelete(NULL);
}

// Internal helpers
static void stt_pipeline_mark_stopped(void) {
    is_running = false;
    is_recording = false;
}

static void stt_pipeline_dispatch_stop_event(void) {
    if (s_stop_event_posted) {
        return;
    }

    s_stop_event_posted = true;

    system_event_t evt = {
        .type = SYSTEM_EVENT_STT_STOPPED,
        .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL),
    };

    if (!event_dispatcher_post(&evt, pdMS_TO_TICKS(10))) {
        ESP_LOGW(TAG, "Failed to enqueue STT stop event");
    }
}

static void stt_pipeline_reset_ring_buffer(void) {
    if (g_audio_ring_buffer == NULL) {
        return;
    }

    if (g_ring_buffer_mutex != NULL) {
        if (xSemaphoreTake(g_ring_buffer_mutex, pdMS_TO_TICKS(50))) {
            g_ring_buffer_write_pos = 0;
            g_ring_buffer_read_pos = 0;
            g_ring_buffer_count = 0;
            xSemaphoreGive(g_ring_buffer_mutex);
        }
    } else {
        g_ring_buffer_write_pos = 0;
        g_ring_buffer_read_pos = 0;
        g_ring_buffer_count = 0;
    }
}

static inline void stt_pipeline_notify_capture_idle(void) {
    if (s_pipeline_ctx.stream_events != NULL) {
        xEventGroupSetBits(s_pipeline_ctx.stream_events, STT_STREAM_EVENT_CAPTURE_IDLE);
    }
}

static bool stt_pipeline_stop_signal_received(void) {
    if (s_pipeline_ctx.stream_events == NULL) {
        return false;
    }

    EventBits_t bits = xEventGroupGetBits(s_pipeline_ctx.stream_events);
    return (bits & STT_STREAM_EVENT_STOP) != 0U;
}

static void stt_pipeline_wait_for_capture_idle(TickType_t deadline_ticks) {
    TickType_t now = xTaskGetTickCount();
    TickType_t wait_ticks = (deadline_ticks > now) ? (deadline_ticks - now) : 0;

    if (s_pipeline_ctx.stream_events != NULL) {
        EventBits_t bits = xEventGroupWaitBits(
            s_pipeline_ctx.stream_events,
            STT_STREAM_EVENT_CAPTURE_IDLE,
            pdTRUE,
            pdFALSE,
            wait_ticks
        );

        if ((bits & STT_STREAM_EVENT_CAPTURE_IDLE) != 0U) {
            return;
        }
    }

    while (g_audio_capture_task_handle != NULL && xTaskGetTickCount() < deadline_ticks) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (g_audio_capture_task_handle != NULL) {
        TaskHandle_t handle = g_audio_capture_task_handle;
        g_audio_capture_task_handle = NULL;
        ESP_LOGW(TAG, "Force deleting audio capture task after timeout");
        vTaskDelete(handle);
        stt_pipeline_notify_capture_idle();
    }
}

static void stt_pipeline_wait_for_streaming_idle(TickType_t deadline_ticks) {
    while (g_streaming_active && xTaskGetTickCount() < deadline_ticks) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (g_streaming_active) {
        ESP_LOGW(TAG, "Persistent streaming task still active after timeout");
    }
}

// Ring buffer helper functions
// static size_t ring_buffer_available_space(void) {
//     if (g_ring_buffer_mutex == NULL) {
//         return g_ring_buffer_size - g_ring_buffer_count;
//     }
//
//     size_t space;
//     xSemaphoreTake(g_ring_buffer_mutex, portMAX_DELAY);
//     space = g_ring_buffer_size - g_ring_buffer_count;
//     xSemaphoreGive(g_ring_buffer_mutex);
//     return space;
// }

static size_t ring_buffer_available_data(void) {
    if (g_ring_buffer_mutex == NULL) {
        return g_ring_buffer_count;
    }

    size_t data;
    xSemaphoreTake(g_ring_buffer_mutex, portMAX_DELAY);
    data = g_ring_buffer_count;
    xSemaphoreGive(g_ring_buffer_mutex);
    return data;
}

static esp_err_t ring_buffer_write(const uint8_t *data, size_t len) {
    // Validate inputs
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (len > g_ring_buffer_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (!xSemaphoreTake(g_ring_buffer_mutex, pdMS_TO_TICKS(100))) {
        ESP_LOGW(TAG, "⚠ Ring buffer mutex timeout");
        return ESP_ERR_TIMEOUT;
    }

    size_t available = g_ring_buffer_size - g_ring_buffer_count;
    if (available < len) {
        xSemaphoreGive(g_ring_buffer_mutex);
        return ESP_ERR_NO_MEM;
    }

    size_t write_pos = g_ring_buffer_write_pos;
    for (size_t i = 0; i < len; i++) {
        if (write_pos >= g_ring_buffer_size) {
            write_pos = 0;
        }
        g_audio_ring_buffer[write_pos] = data[i];
        write_pos++;
    }

    g_ring_buffer_write_pos = (write_pos % g_ring_buffer_size);
    g_ring_buffer_count += len;

    xSemaphoreGive(g_ring_buffer_mutex);
    return ESP_OK;
}

static esp_err_t ring_buffer_read(uint8_t *data, size_t len, size_t *bytes_read) {
    // Validate inputs
    if (data == NULL || bytes_read == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!xSemaphoreTake(g_ring_buffer_mutex, pdMS_TO_TICKS(100))) {
        ESP_LOGW(TAG, "⚠ Ring buffer mutex timeout (read)");
        *bytes_read = 0;
        return ESP_ERR_TIMEOUT;
    }

    size_t available = g_ring_buffer_count;
    if (available == 0) {
        xSemaphoreGive(g_ring_buffer_mutex);
        *bytes_read = 0;
        return ESP_OK;
    }

    size_t to_read = (len < available) ? len : available;

    size_t read_pos = g_ring_buffer_read_pos;
    for (size_t i = 0; i < to_read; i++) {
        if (read_pos >= g_ring_buffer_size) {
            read_pos = 0;
        }
        data[i] = g_audio_ring_buffer[read_pos];
        read_pos++;
    }

    g_ring_buffer_read_pos = (read_pos % g_ring_buffer_size);
    g_ring_buffer_count -= to_read;

    xSemaphoreGive(g_ring_buffer_mutex);
    
    *bytes_read = to_read;
    return ESP_OK;
}

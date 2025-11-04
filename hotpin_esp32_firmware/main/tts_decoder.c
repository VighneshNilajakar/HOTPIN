/**
 * @file tts_decoder.c
 * @brief TTS audio decoder with WAV parsing and I2S playback
 * 
 * Implements:
 * - 44-byte WAV RIFF header parsing
 * - Sample rate, channels, bit depth extraction
 * - PCM data streaming to I2S TX
 * - Multi-chunk WAV file handling
 */

#include "tts_decoder.h"
#include "config.h"
#include "audio_driver.h"
#include "audio_feedback.h"
#include "websocket_client.h"
#include "event_dispatcher.h"
#include "system_events.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include <string.h>
#include <inttypes.h>
#include "esp_timer.h"
#include <string.h>
#include <stdint.h>



static const char *TAG = TAG_TTS;

// Runtime WAV metadata extracted from the RIFF header
typedef struct {
    uint16_t audio_format;   // Should be 1 (PCM)
    uint16_t num_channels;   // 1 = mono, 2 = stereo
    uint32_t sample_rate;    // Samples per second (e.g., 16000)
    uint32_t byte_rate;      // sample_rate * channels * bits_per_sample / 8
    uint16_t block_align;    // channels * bits_per_sample / 8
    uint16_t bits_per_sample;// Typically 16 for PCM
    uint32_t data_size;      // Bytes of PCM data reported by header
} wav_runtime_info_t;

// Stream buffer for audio data
// ✅ CRITICAL FIX: Increased buffer size to handle larger TTS responses
// Server generates responses up to 256KB - previous 192KB buffer was too small
#define TTS_STREAM_BUFFER_SIZE (327680)      // 320 KB - handles typical responses with safety margin
#define TTS_STREAM_BUFFER_TRIGGER_LEVEL (16 * 1024)  // Maintain reasonable trigger threshold for streaming
#define AUDIO_CHUNK_SIZE        4096        // DMA buffer size for I2S playback
static StreamBufferHandle_t g_audio_stream_buffer = NULL;
static uint8_t* g_stream_buffer_storage = NULL; // Storage will be in PSRAM
static StaticStreamBuffer_t s_stream_buffer_struct;

// State management
static bool is_initialized = false;
static volatile bool is_playing = false;
static volatile bool is_running = false;
static volatile bool header_parsed = false;
static volatile bool playback_feedback_sent = false;
static volatile bool eos_requested = false;
static volatile bool playback_completed = false;  // Track when playback is actually completed
static volatile bool audio_data_received = false; // Track if we've received any audio data
static volatile uint32_t playback_start_time = 0; // Track when playback started
static volatile bool is_session_active = false;   // Track if we're in an active audio session
static volatile uint32_t session_start_time = 0;  // Track when current session started
static volatile bool force_stop_requested = false; // Emergency stop flag
static volatile bool session_ended = false;        // Track if current session has ended
static volatile uint32_t session_bytes_played = 0;  // Track bytes played in current session

// WAV header info
static wav_runtime_info_t wav_info;
static volatile size_t bytes_received = 0;
static volatile size_t pcm_bytes_played = 0;

// Stereo duplication scratch space (allocated on demand)
static uint8_t *s_stereo_scratch = NULL;
static size_t s_stereo_scratch_size = 0;
static size_t s_stereo_scratch_capacity_samples = 0;

// Playback task
static TaskHandle_t g_playback_task_handle = NULL;

#define WAV_HEADER_BUFFER_MAX   8192

// Buffer for header accumulation
static uint8_t header_buffer[WAV_HEADER_BUFFER_MAX] __attribute__((aligned(4)));
static volatile size_t header_bytes_received = 0;

// ===========================
// Private Function Declarations
// ===========================
static void tts_playback_task(void *pvParameters);
static esp_err_t parse_wav_header(const uint8_t *buffer, size_t length, size_t *header_consumed);
static void print_wav_info(const wav_runtime_info_t *info);
static void audio_data_callback(const uint8_t *data, size_t len, void *arg);
static esp_err_t write_pcm_chunk_to_driver(const uint8_t *data, size_t length, size_t *accounted_bytes);
static bool ensure_stereo_scratch_buffer(void);

// Safe watchdog reset function to prevent errors when task is not registered
// ✅ FIX #2: Only reset watchdog if we're in the correct task context and still registered
// Suppresses ESP_ERR_INVALID_ARG error during task shutdown race condition
static inline void safe_task_wdt_reset(void) {
    // CRITICAL: Check if current task matches the playback task handle
    // This prevents most cases of calling esp_task_wdt_reset() after unregistration
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    if (g_playback_task_handle != NULL && 
        current_task == g_playback_task_handle && 
        is_running) {
        esp_err_t ret = esp_task_wdt_reset();
        // ✅ FIX #2: Silently ignore ESP_ERR_NOT_FOUND (task not registered) 
        // This occurs during brief race condition when unregistering from watchdog
        // The error is harmless - task is shutting down cleanly
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_ARG && ret != ESP_ERR_NOT_FOUND) {
            // Only log unexpected errors at detailed level to avoid spam
            ESP_LOGD(TAG, "WDT reset failed: %s", esp_err_to_name(ret));
        }
    }
}

// ===========================
// Public Functions
// ===========================

esp_err_t tts_decoder_init(void) {
    ESP_LOGI(TAG, "Initializing TTS decoder...");
    
    if (is_initialized) {
        ESP_LOGW(TAG, "TTS decoder already initialized");
        return ESP_OK;
    }
    
    // Create audio stream buffer in PSRAM
    if (g_audio_stream_buffer == NULL) {
        // Check if sufficient PSRAM is available before allocating
        size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t required = TTS_STREAM_BUFFER_SIZE + 32768; // Buffer + 32KB safety margin
        
        if (psram_free < required) {
            ESP_LOGE(TAG, "Insufficient PSRAM for TTS buffer: need %u bytes, have %u bytes",
                     (unsigned int)required, (unsigned int)psram_free);
            return ESP_ERR_NO_MEM;
        }
        
        ESP_LOGI(TAG, "Allocating %d byte PSRAM buffer for TTS stream", TTS_STREAM_BUFFER_SIZE);
        ESP_LOGI(TAG, "  PSRAM available: %u bytes (%u KB)", 
                 (unsigned int)psram_free, (unsigned int)(psram_free / 1024));
        
        g_stream_buffer_storage = (uint8_t *)heap_caps_malloc(TTS_STREAM_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
        if (g_stream_buffer_storage == NULL) {
            ESP_LOGE(TAG, "Failed to allocate PSRAM for stream buffer storage");
            ESP_LOGE(TAG, "  Requested: %d bytes", TTS_STREAM_BUFFER_SIZE);
            ESP_LOGE(TAG, "  Available: %u bytes", (unsigned int)psram_free);
            return ESP_ERR_NO_MEM;
        }
        
        ESP_LOGI(TAG, "  ✓ TTS stream buffer allocated at %p", g_stream_buffer_storage);

        g_audio_stream_buffer = xStreamBufferCreateStatic(
            TTS_STREAM_BUFFER_SIZE,
            TTS_STREAM_BUFFER_TRIGGER_LEVEL,
            g_stream_buffer_storage,
            &s_stream_buffer_struct
        );

        if (g_audio_stream_buffer == NULL) {
            ESP_LOGE(TAG, "Failed to create audio stream buffer");
            heap_caps_free(g_stream_buffer_storage);
            g_stream_buffer_storage = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
    
    // Register audio callback with WebSocket client
    websocket_client_set_audio_callback(audio_data_callback, NULL);
    
    is_initialized = true;
    ESP_LOGI(TAG, "✅ TTS decoder initialized");
    
    return ESP_OK;
}

esp_err_t tts_decoder_deinit(void) {
    ESP_LOGI(TAG, "Deinitializing TTS decoder...");
    
    if (!is_initialized) {
        ESP_LOGW(TAG, "TTS decoder not initialized");
        return ESP_OK;
    }
    
    // Stop playback if active
    if (is_playing) {
        tts_decoder_stop();
    }
    
    // Delete stream buffer
    if (g_audio_stream_buffer != NULL) {
        vStreamBufferDelete(g_audio_stream_buffer);
        g_audio_stream_buffer = NULL;
    }
    if (g_stream_buffer_storage != NULL) {
        heap_caps_free(g_stream_buffer_storage);
        g_stream_buffer_storage = NULL;
    }

    if (s_stereo_scratch != NULL) {
        ESP_LOGD(TAG, "Freeing stereo scratch buffer (%zu bytes)", s_stereo_scratch_size);
        heap_caps_free(s_stereo_scratch);
        s_stereo_scratch = NULL;
        s_stereo_scratch_size = 0;
        s_stereo_scratch_capacity_samples = 0;
    }
    
    is_initialized = false;
    ESP_LOGI(TAG, "TTS decoder deinitialized");
    
    return ESP_OK;
}

esp_err_t tts_decoder_start(void) {
    ESP_LOGI(TAG, "🎵 Starting TTS decoder...");

    if (!is_initialized) {
        ESP_LOGE(TAG, "TTS decoder not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // ✅ SELF-HEALING: Check for stale state from previous failed shutdown
    if (g_playback_task_handle != NULL || is_running) {
        ESP_LOGW(TAG, "TTS decoder appears to be in a stale state. Forcing a stop before starting.");
        tts_decoder_stop(); // Call the forceful stop function
        vTaskDelay(pdMS_TO_TICKS(50)); // Allow time for cleanup
    }

    // CRITICAL CHECK: Verify I2S driver is initialized before starting playback
    // This prevents ESP_ERR_INVALID_STATE when audio arrives after mode transition
    if (!audio_driver_is_initialized()) {
        ESP_LOGW(TAG, "Cannot start TTS decoder - I2S driver not initialized (likely in camera mode)");
        ESP_LOGW(TAG, "Audio will be buffered but not played until voice mode is re-entered");
        return ESP_ERR_INVALID_STATE;
    }

    // Initialize decoder
    if (tts_decoder_init() != ESP_OK) {
        return ESP_FAIL;
    }

    // Reset state
    header_parsed = false;
    header_bytes_received = 0;
    bytes_received = 0;
    pcm_bytes_played = 0;
    playback_feedback_sent = false;
    eos_requested = false;
    playback_completed = false;
    audio_data_received = false;
    playback_start_time = (uint32_t)(esp_timer_get_time() / 1000);
    memset(&wav_info, 0, sizeof(wav_info));

    // Ensure no stale PCM data remains from a previous session
    if (g_audio_stream_buffer != NULL) {
        xStreamBufferReset(g_audio_stream_buffer);
    }

    // CRITICAL FIX: Move TTS playback task to Core 1 to prevent Core 0 starvation
    // Core 0 handles WiFi/TCP and STT input, Core 1 handles state management and TTS output
    // IMPORTANT: Add safety padding to prevent buffer overflows
    ESP_LOGI(TAG, "[CORE AFFINITY] Creating TTS playback task on Core 1 (APP_CPU) with safety measures");
    BaseType_t ret = xTaskCreatePinnedToCore(
        tts_playback_task,
        "tts_playback",
        TASK_STACK_SIZE_LARGE,  // Increased from TASK_STACK_SIZE_MEDIUM + 1024 to prevent stack overflow
        NULL,
        TASK_PRIORITY_TTS_DECODER,
        &g_playback_task_handle,
        TASK_CORE_CONTROL  // Core 1 - Balance load across cores to prevent watchdog timeout
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create playback task");
        return ESP_FAIL;
    }

    // NOTE: Watchdog registration moved inside tts_playback_task() to avoid race condition
    // The task registers itself immediately after starting

    esp_err_t clk_ret = audio_driver_set_tx_sample_rate(CONFIG_AUDIO_SAMPLE_RATE);
    if (clk_ret != ESP_OK) {
        ESP_LOGW(TAG, "Unable to reset TX sample rate at decoder start: %s", esp_err_to_name(clk_ret));
    }

    is_running = true;
    is_playing = true;
    is_session_active = true;  // ✅ FIX: Mark session active when decoder starts expecting audio

    system_event_t evt = {
        .type = SYSTEM_EVENT_TTS_PLAYBACK_STARTED,
        .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL),
    };
    if (!event_dispatcher_post(&evt, pdMS_TO_TICKS(10))) {
        ESP_LOGW(TAG, "Failed to enqueue TTS playback start event");
    }

    ESP_LOGI(TAG, "✅ TTS decoder started successfully");
    return ESP_OK;
}

esp_err_t tts_decoder_stop(void) {
    ESP_LOGI(TAG, "⏹️ Stopping TTS decoder...");

    if (!is_running && g_playback_task_handle == NULL) {
        ESP_LOGW(TAG, "TTS decoder already stopped.");
        // Still perform full state reset to ensure clean slate
        header_parsed = false;
        playback_feedback_sent = false;
        eos_requested = false;
        playback_completed = false;
        audio_data_received = false;
        is_session_active = false;
        session_ended = false;
        force_stop_requested = false;
        bytes_received = 0;
        pcm_bytes_played = 0;
        header_bytes_received = 0;
        session_bytes_played = 0;
        playback_start_time = 0;
        session_start_time = 0;
        // Clear the stream buffer if it exists
        if (g_audio_stream_buffer != NULL) {
            xStreamBufferReset(g_audio_stream_buffer);
        }
        return ESP_OK;
    }

    // Immediately signal all loops to stop
    is_playing = false;
    is_running = false;
    eos_requested = true;
    force_stop_requested = true;

    // ✅ CRITICAL FIX: Unblock the stream buffer BEFORE deleting the task
    // If the task is blocked in xStreamBufferReceive(), send dummy data to unblock it
    // This allows the task to see the stop flags and exit cleanly
    if (g_playback_task_handle != NULL && g_audio_stream_buffer != NULL) {
        ESP_LOGI(TAG, "Unblocking stream buffer to allow task cleanup...");
        // Send a zero byte to wake up any blocked receiver
        uint8_t dummy = 0;
        xStreamBufferSend(g_audio_stream_buffer, &dummy, 1, 0);
        
        // Give the task a brief moment to notice the stop flags and exit cleanly
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Forcefully terminate the playback task if it's still running
    if (g_playback_task_handle != NULL) {
        ESP_LOGW(TAG, "Forcefully deleting active playback task.");
        TaskHandle_t temp_handle = g_playback_task_handle;
        g_playback_task_handle = NULL;  // Nullify handle before deletion
        vTaskDelete(temp_handle);
        
        // ✅ CRITICAL: Small delay after vTaskDelete to ensure scheduler cleanup
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    // ✅ CRITICAL FIX: Reset stream buffer AFTER task deletion to clear dangling state
    // This prevents "xTaskWaitingToReceive" assertion failure on next start
    if (g_audio_stream_buffer != NULL) {
        ESP_LOGI(TAG, "Resetting audio stream buffer to clear internal state.");
        xStreamBufferReset(g_audio_stream_buffer);
    }

    // Restore the default I2S clock rate as a safety measure
    esp_err_t clk_ret = audio_driver_set_tx_sample_rate(CONFIG_AUDIO_SAMPLE_RATE);
    if (clk_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to restore TX sample rate during stop: %s", esp_err_to_name(clk_ret));
    }

    // --- CRITICAL: Full State Reset ---
    ESP_LOGI(TAG, "Performing full state reset of TTS decoder.");
    header_parsed = false;
    playback_feedback_sent = false;
    eos_requested = false;
    playback_completed = false;
    audio_data_received = false;
    is_session_active = false;
    session_ended = false;
    force_stop_requested = false;
    bytes_received = 0;
    pcm_bytes_played = 0;
    header_bytes_received = 0;
    session_bytes_played = 0;
    playback_start_time = 0;
    session_start_time = 0;
    
    // Free stereo scratch buffer to prevent DMA fragmentation
    if (s_stereo_scratch != NULL) {
        ESP_LOGI(TAG, "Freeing stereo scratch buffer (%zu bytes) to reduce fragmentation", s_stereo_scratch_size);
        heap_caps_free(s_stereo_scratch);
        s_stereo_scratch = NULL;
        s_stereo_scratch_size = 0;
        s_stereo_scratch_capacity_samples = 0;
    }
    // ------------------------------------

    ESP_LOGI(TAG, "⏹️ TTS decoder stopped and reset.");
    return ESP_OK;
}

bool tts_decoder_is_playing(void) {
    return is_playing;
}

bool tts_decoder_is_running(void) {
    return is_running;
}

bool tts_decoder_is_receiving_audio(void) {
    // Return true if:
    // 1. Decoder is running (task active)
    // 2. We've received audio data in this session
    // 3. Playback hasn't completed yet
    // 4. Session is still active
    return is_running && audio_data_received && !playback_completed && is_session_active;
}

// ===========================
// Private Functions
// ===========================

static void audio_data_callback(const uint8_t *data, size_t len, void *arg) {
    static uint32_t chunk_count = 0;
    static uint32_t rejected_count = 0;
    static uint32_t last_log_count = 0;
    chunk_count++;

    // ✅ FIX #5: Reject audio data if TTS decoder is not running
    // This prevents buffer overflow when server sends audio after mode transition
    if (!is_running || g_playback_task_handle == NULL) {
        rejected_count++;

        // ✅ FIX: Further reduce log spam - only log first, every 50th, and reset on new session
        // This prevents hundreds of warnings during mode transitions
        if (rejected_count == 1 ||
            (rejected_count >= 50 && (rejected_count % 50) == 0) ||
            (rejected_count - last_log_count) >= 100) {
            ESP_LOGW(TAG, "Rejecting audio chunks - TTS decoder not running (chunks rejected: %u, last: %zu bytes)",
                     (unsigned int)rejected_count, len);
            last_log_count = rejected_count;
        }
        return;
    }

    // Clear rejection counters once the decoder starts processing audio again
    if (rejected_count != 0) {
        rejected_count = 0;
        last_log_count = 0;
    }
    
    // ✅ BUFFER OVERFLOW DETECTION: Check buffer capacity BEFORE accepting data
    size_t buffer_space = xStreamBufferSpacesAvailable(g_audio_stream_buffer);
    size_t buffer_used = xStreamBufferBytesAvailable(g_audio_stream_buffer);
    
    ESP_LOGI(TAG, "Received audio chunk #%u: %zu bytes (buffer: %zu/%d used, %zu free)", 
             (unsigned int)chunk_count, len, buffer_used, TTS_STREAM_BUFFER_SIZE, buffer_space);
    
    // ✅ CRITICAL: Warn if incoming data exceeds available buffer space
    if (len > buffer_space) {
        ESP_LOGW(TAG, "⚠️ BUFFER PRESSURE: Incoming %zu bytes, only %zu bytes free", len, buffer_space);
        ESP_LOGW(TAG, "   Buffer: %zu/%d bytes used (%.1f%% full)", 
                 buffer_used, TTS_STREAM_BUFFER_SIZE, 
                 (buffer_used * 100.0) / TTS_STREAM_BUFFER_SIZE);
        ESP_LOGW(TAG, "   This may cause delays or data loss if playback is slow");
    }
    
    // Log first few bytes of EVERY chunk for debugging WAV stream issues
    if (chunk_count <= 5 && len >= 12) {
        ESP_LOGI(TAG, "Chunk #%u first 12 bytes: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                 (unsigned int)chunk_count,
                 data[0], data[1], data[2], data[3], data[4], data[5], 
                 data[6], data[7], data[8], data[9], data[10], data[11]);
    }
    
    // Reset chunk counter when starting a new session
    if (!audio_data_received && chunk_count > 1) {
        ESP_LOGI(TAG, "New audio session detected - resetting chunk counter");
        chunk_count = 1;
    }

    // ✅ FIX: Handle zero-length chunks early - don't mark as audio data received
    if (g_audio_stream_buffer != NULL) {
        // Handle special case: zero-length data with NULL pointer (used to signal EOS)
        if (len == 0) {
            if (data == NULL) {
                ESP_LOGD(TAG, "Received EOS signal (NULL data, zero length)");
                // Just return without sending to stream buffer for NULL zero-length data
                return;
            }
            // ✅ FIX: Also return early for zero-length non-NULL data (connection close signal)
            ESP_LOGD(TAG, "Received zero-length chunk (likely connection event) - ignoring");
            return;
        } else {
            // For non-zero length, data must not be NULL
            if (data == NULL) {
                ESP_LOGE(TAG, "Received non-zero length (%zu bytes) with NULL data pointer", len);
                return;
            }
        }

        // Push the entire chunk into the stream buffer, yielding while we wait for space
        const TickType_t per_attempt_wait = pdMS_TO_TICKS(40);
        const TickType_t max_wait_ticks = pdMS_TO_TICKS(1000);  // Allow up to 1s per chunk before giving up
        TickType_t wait_start = xTaskGetTickCount();
        size_t total_sent = 0;

        while (total_sent < len) {
            size_t remaining = len - total_sent;
            size_t sent = xStreamBufferSend(g_audio_stream_buffer,
                                            data + total_sent,
                                            remaining,
                                            per_attempt_wait);

            if (sent == 0) {
                if ((xTaskGetTickCount() - wait_start) >= max_wait_ticks) {
                    static uint32_t timeout_count = 0;
                    timeout_count++;
                    uint32_t waited_ms = (uint32_t)((xTaskGetTickCount() - wait_start) * portTICK_PERIOD_MS);
                    ESP_LOGW(TAG, "Stream buffer congested - dropped %zu bytes after %u ms (timeouts: %u)",
                             remaining,
                             (unsigned int)waited_ms,
                             (unsigned int)timeout_count);
                    break;
                }

                // Give the playback task time to drain while keeping watchdog happy
                safe_task_wdt_reset();
                continue;
            }

            total_sent += sent;
        }

        if (total_sent != len) {
            // Chunk was not fully enqueued - treat remaining bytes as dropped
            return;
        }

        bytes_received += len;

        // ✅ FIX: Mark audio data received only AFTER successfully sending data to buffer
        if (!audio_data_received) {
            audio_data_received = true;
            ESP_LOGI(TAG, "🎙️ First real audio data received for session (%zu bytes)", len);

            // Send session start notification for the first audio chunk
            system_event_t evt = {
                .type = SYSTEM_EVENT_TTS_PLAYBACK_STARTED,
                .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000),
            };
            if (!event_dispatcher_post(&evt, pdMS_TO_TICKS(10))) {
                ESP_LOGW(TAG, "Failed to enqueue TTS playback start event");
            }
        }

        static uint32_t success_count = 0;
        success_count++;
        if ((success_count % 100) == 0) {  // Log every 100 successes to prevent log spam
            ESP_LOGD(TAG, "Sent %zu bytes to stream buffer (total received: %zu, successes: %u)",
                     len, bytes_received, (unsigned int)success_count);
        }

        // Reset playback completed flag when new data arrives
        if (playback_completed) {
            ESP_LOGD(TAG, "New audio data received, resetting playback completed flag");
            playback_completed = false;
        }
    } else {
        // Only log when decoder is not initialized at all
        static uint32_t drop_count = 0;
        drop_count++;
        if ((drop_count % 50) == 0) {  // Log every 50 drops to prevent log spam
            ESP_LOGW(TAG, "Decoder not initialized - dropping %zu-byte chunk (drops: %u)", len, (unsigned int)drop_count);
        }
    }
}

static void tts_playback_task(void *pvParameters) {
    ESP_LOGI(TAG, "🎵 TTS playback task started on Core %d", xPortGetCoreID());

    // Register with watchdog FIRST THING after task starts
    esp_err_t wdt_ret = esp_task_wdt_add(NULL);  // NULL = current task
    if (wdt_ret == ESP_OK) {
        ESP_LOGI(TAG, "✅ TTS playback task registered with watchdog");
    } else if (wdt_ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGD(TAG, "TTS task already registered with watchdog");
    } else {
        ESP_LOGW(TAG, "Failed to register TTS task with watchdog: %s", esp_err_to_name(wdt_ret));
    }

    // CRITICAL SAFETY CHECK: Prevent InstructionFetchError crash from PSRAM execution
    uint32_t pc_check = (uint32_t)__builtin_return_address(0);
    if ((pc_check >= 0x3F800000) && (pc_check < 0x40000000)) {
        ESP_LOGE(TAG, "❌ EMERGENCY ABORT: TTS task executing from PSRAM (0x%08x) - preventing crash!", (unsigned int)pc_check);
        
        // Unregister from watchdog before exit
        esp_task_wdt_delete(NULL);
        
        g_playback_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    // CRITICAL FIX: Allocate DMA buffer in PSRAM instead of on stack to prevent stack overflow
    // Stack allocation of 4KB buffer was causing stack overflow in tts_playback task
    uint8_t *dma_buffer = heap_caps_malloc(AUDIO_CHUNK_SIZE, MALLOC_CAP_SPIRAM);
    if (dma_buffer == NULL) {
        ESP_LOGE(TAG, "❌ Failed to allocate %d-byte DMA buffer in PSRAM", AUDIO_CHUNK_SIZE);
        ESP_LOGE(TAG, "  Free PSRAM: %u bytes", (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        g_playback_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "  ✓ DMA buffer allocated in PSRAM at %p (%d bytes)", dma_buffer, AUDIO_CHUNK_SIZE);
    
    esp_err_t playback_result = ESP_OK;
    uint32_t last_activity_timestamp = (uint32_t)(esp_timer_get_time() / 1000);
    uint8_t consecutive_i2s_failures = 0;  // Track consecutive I2S errors for graceful degradation

    while (is_running) {
        // ✅ FIX #8: Check for stop request at top of loop BEFORE blocking on buffer receive
        // This ensures fast response to tts_decoder_stop() even when actively playing
        if (!is_running || force_stop_requested) {
            ESP_LOGI(TAG, "Playback task stop requested - exiting main loop");
            playback_completed = true;
            break;
        }
        
        // Wait for data in the stream buffer
        size_t bytes_received_from_stream = xStreamBufferReceive(
            g_audio_stream_buffer,
            dma_buffer,
            AUDIO_CHUNK_SIZE,
            pdMS_TO_TICKS(100) // 100ms timeout - short enough to check stop flags frequently
        );

        if (bytes_received_from_stream > 0) {
            last_activity_timestamp = (uint32_t)(esp_timer_get_time() / 1000);
            
            // ✅ FIX #9: Reset watchdog during active audio processing to prevent timeout errors
            // This is critical when receiving and processing large amounts of audio data
            static uint32_t wdt_reset_counter = 0;
            wdt_reset_counter++;
            if (wdt_reset_counter % 10 == 0) {  // Reset every 10 iterations (~1 second of audio)
                safe_task_wdt_reset();
            }
            
            // ✅ FIX: Check if all audio data has been received
            // Compare bytes_received against expected total (WAV header + data size)
            if (header_parsed && bytes_received >= (wav_info.data_size + 44)) {
                ESP_LOGI(TAG, "✅ All audio data received (%zu bytes, expected %u + 44 header)", 
                         bytes_received, (unsigned int)wav_info.data_size);
                // Continue processing this chunk, then exit on next iteration
                eos_requested = true;
            }
            
            // We have data, process it
            static uint32_t buffer_monitor_count = 0;
            buffer_monitor_count++;
            
            // Monitor buffer levels periodically to detect overflow conditions early
            if ((buffer_monitor_count % 50) == 0) {  // Check every 50 iterations
                size_t buffer_space = xStreamBufferSpacesAvailable(g_audio_stream_buffer);
                size_t buffer_level = xStreamBufferBytesAvailable(g_audio_stream_buffer);
                ESP_LOGD(TAG, "[BUFFER MONITOR] Level: %zu bytes | Space: %zu bytes | Ratio: %.2f%%", 
                         buffer_level, buffer_space, 
                         (buffer_level * 100.0) / (buffer_level + buffer_space));
                         
                // Warn if buffer is getting full
                if (buffer_level > (TTS_STREAM_BUFFER_SIZE * 0.8)) {  // 80% full
                    ESP_LOGW(TAG, "⚠ Stream buffer approaching capacity: %zu/%d bytes (%.1f%%)", 
                             buffer_level, TTS_STREAM_BUFFER_SIZE, 
                             (buffer_level * 100.0) / TTS_STREAM_BUFFER_SIZE);
                }
            }
            
            // Adaptive flow control - slow down when buffer is getting full
            size_t current_level = xStreamBufferBytesAvailable(g_audio_stream_buffer);
            if (current_level > (TTS_STREAM_BUFFER_SIZE * 0.75)) {
                // Buffer is 75% full - add small delay to let processing catch up
                vTaskDelay(pdMS_TO_TICKS(2));
            } else if (current_level > (TTS_STREAM_BUFFER_SIZE * 0.9)) {
                // Buffer is 90% full - add larger delay to prevent overflow
                vTaskDelay(pdMS_TO_TICKS(5));
            }
            
            if (!header_parsed) {
                // Still parsing header - accumulate into header_buffer
                if (header_bytes_received + bytes_received_from_stream > WAV_HEADER_BUFFER_MAX) {
                    ESP_LOGE(TAG, "Header staging buffer overflow (%zu + %zu)",
                             header_bytes_received, bytes_received_from_stream);
                    is_running = false;
                    playback_result = ESP_ERR_INVALID_SIZE;
                    break;
                }

                memcpy(header_buffer + header_bytes_received, dma_buffer, bytes_received_from_stream);
                header_bytes_received += bytes_received_from_stream;

                size_t header_consumed = 0;
                esp_err_t ret = parse_wav_header(header_buffer, header_bytes_received, &header_consumed);
                if (ret == ESP_OK) {
                    header_parsed = true;
                    print_wav_info(&wav_info);

                    if (!playback_feedback_sent) {
                        esp_err_t fb_ret = audio_feedback_beep_single(false);
                        if (fb_ret != ESP_OK) {
                            ESP_LOGW(TAG, "Playback start feedback failed: %s", esp_err_to_name(fb_ret));
                        } else {
                            ESP_LOGI(TAG, "🔔 Playback start feedback dispatched (bytes_received=%zu)", bytes_received);
                        }
                        playback_feedback_sent = true;
                    }

                    esp_err_t clk_ret = audio_driver_set_tx_sample_rate(wav_info.sample_rate);
                    if (clk_ret != ESP_OK) {
                        ESP_LOGW(TAG, "Unable to set TX sample rate to %u Hz: %s",
                                 (unsigned int)wav_info.sample_rate, esp_err_to_name(clk_ret));
                    }

                    // Play any remaining PCM data from the header buffer
                    size_t pcm_len = header_bytes_received - header_consumed;
                    if (pcm_len > 0) {
                        // Check I2S state before attempting write
                        if (!audio_driver_is_initialized()) {
                            ESP_LOGW(TAG, "I2S deinitialized before initial PCM write - aborting playback");
                            is_running = false;
                            playback_completed = true;
                            playback_result = ESP_ERR_INVALID_STATE;
                            break;
                        }
                        
                        size_t accounted = 0;
                        esp_err_t write_ret = write_pcm_chunk_to_driver(header_buffer + header_consumed,
                                                                       pcm_len,
                                                                       &accounted);
                        if (write_ret == ESP_OK) {
                            pcm_bytes_played += accounted;
                            ESP_LOGD(TAG, "Played %zu bytes from initial chunk (total: %zu)",
                                     accounted, pcm_bytes_played);
                        } else if (write_ret == ESP_ERR_INVALID_STATE) {
                            ESP_LOGW(TAG, "Initial PCM write failed - I2S deinitialized: %s", esp_err_to_name(write_ret));
                            is_running = false;
                            playback_completed = true;
                            playback_result = write_ret;
                            break;
                        } else {
                            ESP_LOGE(TAG, "Initial PCM write failed: %s", esp_err_to_name(write_ret));
                            playback_result = write_ret;
                            is_running = false;
                            break;
                        }
                    }

                    header_bytes_received = 0;  // Reset buffer for future sessions
                } else if (ret == ESP_ERR_INVALID_SIZE) {
                    // Need more data to complete header parsing
                    ESP_LOGD(TAG, "Awaiting more header bytes (%zu collected)", header_bytes_received);
                } else if (ret == ESP_ERR_INVALID_ARG) {
                    // ✅ STREAMING FIX: Header not found yet - keep accumulating
                    // In streaming scenarios, the WAV header can arrive in ANY chunk, not just the first
                    // PCM data might arrive before the header due to network fragmentation
                    if (header_bytes_received < WAV_HEADER_BUFFER_MAX - AUDIO_CHUNK_SIZE) {
                        // Still have room in accumulation buffer - keep waiting for header
                        ESP_LOGD(TAG, "⏳ WAV header not found yet - accumulating data (%zu/%d bytes collected)", 
                                 header_bytes_received, WAV_HEADER_BUFFER_MAX);
                        ESP_LOGD(TAG, "   First 4 bytes: 0x%02X 0x%02X 0x%02X 0x%02X (looking for 'RIFF')", 
                                 header_buffer[0], header_buffer[1], header_buffer[2], header_buffer[3]);
                    } else {
                        // Buffer nearly full and still no valid header - this is a fatal error
                        ESP_LOGE(TAG, "❌ Failed to find WAV header after accumulating %zu bytes", header_bytes_received);
                        ESP_LOGE(TAG, "   First 4 bytes: 0x%02X 0x%02X 0x%02X 0x%02X", 
                                 header_buffer[0], header_buffer[1], header_buffer[2], header_buffer[3]);
                        ESP_LOGE(TAG, "   This suggests the stream is not a valid WAV file");
                        is_running = false;
                        playback_result = ret;
                        break;
                    }
                } else {
                    // Other fatal parse error
                    ESP_LOGE(TAG, "Failed to parse WAV header: %s (bytes collected: %zu)", 
                             esp_err_to_name(ret), header_bytes_received);
                    is_running = false;
                    playback_result = ret;
                    break;
                }
            } else {
                // Header already parsed - play PCM data directly from dma_buffer
                if (!playback_feedback_sent) {
                    esp_err_t fb_ret = audio_feedback_beep_single(false);
                    if (fb_ret != ESP_OK) {
                        ESP_LOGW(TAG, "Delayed playback feedback failed: %s", esp_err_to_name(fb_ret));
                    } else {
                        ESP_LOGI(TAG, "🔔 Playback start feedback dispatched (late)");
                    }
                    playback_feedback_sent = true;
                }

                size_t accounted = 0;
                esp_err_t ret = write_pcm_chunk_to_driver(dma_buffer, bytes_received_from_stream, &accounted);

                if (ret == ESP_OK) {
                    pcm_bytes_played += accounted;
                    consecutive_i2s_failures = 0;  // Reset failure counter on success
                    ESP_LOGD(TAG, "Played %zu bytes (total: %zu)", accounted, pcm_bytes_played);
                } else if (ret == ESP_ERR_INVALID_STATE || ret == ESP_ERR_TIMEOUT) {
                    // I2S driver not available - could be during mode transition or re-initialization
                    consecutive_i2s_failures++;
                    ESP_LOGW(TAG, "PCM write failed (%s) - I2S unavailable (attempt %d/5), will retry", 
                             esp_err_to_name(ret), consecutive_i2s_failures);
                    
                    if (consecutive_i2s_failures > 5) {
                        ESP_LOGE(TAG, "I2S persistently unavailable after %d attempts - stopping playback", 
                                 consecutive_i2s_failures);
                        is_running = false;
                        playback_completed = true;
                        playback_result = ret;
                        break;
                    }
                    
                    // Small delay to allow I2S to stabilize if it's being re-initialized
                    vTaskDelay(pdMS_TO_TICKS(50));
                } else {
                    ESP_LOGE(TAG, "Audio playback error: %s", esp_err_to_name(ret));
                    playback_result = ret;
                    is_running = false;
                    break;
                }
            }
        } else {
            // ✅ STABILITY FIX: Timeout occurred - prevent watchdog starvation during network failures
            uint32_t current_time = (uint32_t)(esp_timer_get_time() / 1000);
            
            // CRITICAL: Reset watchdog during idle periods to prevent timeout
            // This is essential when waiting for audio that may never arrive due to network errors
            safe_task_wdt_reset();
            
            // Check if we've been idle for too long or if shutdown is requested
            if (!is_running) {
                ESP_LOGI(TAG, "TTS playback task shutting down - is_running=0");
                break;
            }
            
            // ✅ STABILITY FIX: Shorter timeout during header parsing (5 seconds)
            // During header parsing, we expect data quickly. If it doesn't arrive, likely a network issue.
            if (!header_parsed && (current_time - last_activity_timestamp) > 5000) {
                ESP_LOGW(TAG, "⚠️ No audio data received for 5+ seconds while waiting for header");
                ESP_LOGW(TAG, "   This suggests a network disconnection. Exiting playback task gracefully.");
                ESP_LOGW(TAG, "   websocket_connection_task will handle reconnection automatically.");
                playback_completed = true;
                playback_result = ESP_ERR_TIMEOUT;
                break;
            }
            
            // ✅ STABILITY FIX: Medium timeout after header parsed (10 seconds)
            // Once header is parsed, we expect continuous audio stream. 10s gap = network problem.
            if (header_parsed && (current_time - last_activity_timestamp) > 10000) {
                ESP_LOGW(TAG, "⚠️ No audio data received for 10+ seconds after header parsed");
                ESP_LOGW(TAG, "   This suggests a network disconnection during audio transfer.");
                ESP_LOGW(TAG, "   Exiting playback task gracefully to prevent watchdog timeout.");
                playback_completed = true;
                playback_result = ESP_ERR_TIMEOUT;
                break;
            }
            
            // Original 20-second timeout for final cleanup checks
            if ((current_time - last_activity_timestamp) > 20000) {
                ESP_LOGD(TAG, "Playback task idle for 20+ seconds, checking exit conditions...");
                
                // ✅ FIX #6: If EOS was requested, check if buffer is empty OR has remnant bytes (< 100)
                if (eos_requested) {
                    size_t buffer_remaining = xStreamBufferBytesAvailable(g_audio_stream_buffer);
                    
                    if (buffer_remaining == 0 || buffer_remaining < 100) {
                        if (buffer_remaining > 0) {
                            ESP_LOGI(TAG, "EOS requested with %zu remnant bytes (< 100) after timeout. Exiting playback task.", 
                                     buffer_remaining);
                        } else {
                            ESP_LOGI(TAG, "EOS requested and stream buffer is empty after timeout. Exiting playback task.");
                        }
                        eos_requested = false;
                        playback_completed = true;
                        break;
                    }
                }
                
                // ✅ FIX #8: Even without EOS, exit if buffer has remnant bytes (< 100) and no new data for 1+ second
                // This handles user interruption case where is_running=false but loop is still processing
                size_t buffer_remaining = xStreamBufferBytesAvailable(g_audio_stream_buffer);
                if (buffer_remaining > 0 && buffer_remaining < 100 && (current_time - last_activity_timestamp) > 1000) {
                    ESP_LOGI(TAG, "Buffer stuck with %zu remnant bytes (< 100) for 1+ second. Exiting playback task.", 
                             buffer_remaining);
                    playback_completed = true;
                    break;
                }
                
                // ✅ FIX #10: Exit if no audio data received after 20 seconds (increased from 5 to allow for LLM processing time)
                // STT→LLM→TTS pipeline can take 6-10 seconds, so 5 seconds was too short
                if (!audio_data_received) {
                    ESP_LOGI(TAG, "No audio data received after 20+ seconds. Exiting playback task (likely connection issue).");
                    playback_completed = true;
                    break;
                }
                
                // If we have received data but playback isn't active, something is wrong
                if (!is_playing) {
                    ESP_LOGI(TAG, "Playback not active after 5+ seconds. Exiting playback task.");
                    playback_completed = true;
                    break;
                }
                
                // Reset the timestamp to prevent continuous logging
                last_activity_timestamp = current_time;
            }
            
            // Regular EOS check
            if (eos_requested) {
                size_t buffer_remaining = xStreamBufferBytesAvailable(g_audio_stream_buffer);
                
                // ✅ FIX #6: Exit if buffer is empty OR has remnant bytes (< 100) that won't play
                // Remnant bytes (typically 44-80 bytes) are smaller than minimum I2S DMA buffer size
                // and will never be consumed by the audio driver
                if (buffer_remaining == 0 || buffer_remaining < 100) {
                    if (buffer_remaining > 0) {
                        ESP_LOGI(TAG, "EOS requested with %zu remnant bytes (< 100, won't play). Exiting playback task.", 
                                 buffer_remaining);
                    } else {
                        ESP_LOGI(TAG, "EOS requested and stream buffer is empty. Exiting playback task.");
                    }
                    eos_requested = false;
                    playback_completed = true;
                    break;
                }
            }
        }
    }
    
    ESP_LOGI(TAG, "🎵 TTS playback task exiting (played %zu bytes, result: %s)", 
             pcm_bytes_played, esp_err_to_name(playback_result));
    
    // Play completion feedback if playback was successful (played significant audio)
    // This signals to user that response is complete and they can provide next input
    if (playback_result == ESP_OK && pcm_bytes_played > 10000) {  // > 10KB indicates real content played
        ESP_LOGI(TAG, "Playing TTS completion feedback to signal readiness for next input");
        esp_err_t fb_ret = audio_feedback_beep_triple(false);  // Triple beep: "Ready for next input"
        if (fb_ret != ESP_OK) {
            ESP_LOGW(TAG, "TTS completion feedback failed: %s", esp_err_to_name(fb_ret));
        }
        // Small delay to let feedback complete before cleanup
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    
    // CRITICAL FIX: Free PSRAM-allocated DMA buffer before task exits
    if (dma_buffer != NULL) {
        heap_caps_free(dma_buffer);
        dma_buffer = NULL;
        ESP_LOGI(TAG, "  ✓ DMA buffer freed from PSRAM");
    }
    
    // CRITICAL FIX: Free stereo scratch buffer on task exit to prevent DMA fragmentation
    // This ensures the buffer is freed even if tts_decoder_stop() wasn't called properly
    if (s_stereo_scratch != NULL) {
        ESP_LOGI(TAG, "  ✓ Freeing stereo scratch buffer (%zu bytes) on task exit", s_stereo_scratch_size);
        heap_caps_free(s_stereo_scratch);
        s_stereo_scratch = NULL;
        s_stereo_scratch_size = 0;
        s_stereo_scratch_capacity_samples = 0;
    }
    
    // Clear stream buffer to prevent data accumulation between sessions
    if (g_audio_stream_buffer != NULL) {
        size_t buffer_remaining = xStreamBufferBytesAvailable(g_audio_stream_buffer);
        if (buffer_remaining > 0) {
            ESP_LOGI(TAG, "  ✓ Clearing %zu bytes from stream buffer", buffer_remaining);
            xStreamBufferReset(g_audio_stream_buffer);
        }
    }
    
    // ✅ FIX #1: Unregister from watchdog BEFORE setting any completion flags
    // This MUST happen before setting g_playback_task_handle = NULL
    wdt_ret = esp_task_wdt_delete(NULL);  // Reuse wdt_ret from earlier
    if (wdt_ret == ESP_OK) {
        ESP_LOGD(TAG, "TTS playback task unregistered from watchdog");
    } else if (wdt_ret != ESP_ERR_INVALID_ARG) {
        ESP_LOGD(TAG, "Failed to unregister TTS task from watchdog: %s", esp_err_to_name(wdt_ret));
    }
    
    // ✅ FIX #1: Set task handle to NULL IMMEDIATELY after unregister
    // Save handle first so we can delete the task at the end
    TaskHandle_t temp_handle = g_playback_task_handle;
    g_playback_task_handle = NULL;  // This stops safe_task_wdt_reset() from being called
    
    // Now set all completion flags - wait_for_idle() will see NULL handle and exit immediately
    is_running = false;
    playback_completed = true;
    is_playing = false;
    
    ESP_LOGI(TAG, "  ✓ Watchdog unregistered, handle cleared, flags set");
    
    // Delete task using saved handle
    vTaskDelete(temp_handle);
}

static esp_err_t write_pcm_chunk_to_driver(const uint8_t *data, size_t length, size_t *accounted_bytes) {
    static uint32_t s_duplication_logs = 0;
    static uint32_t s_passthrough_logs = 0;
    if (accounted_bytes) {
        *accounted_bytes = 0;
    }

    if (data == NULL || length == 0) {
        return ESP_OK;
    }

    size_t accounted = length;
    bool duplicate_to_stereo = false;

    if (header_parsed && wav_info.num_channels == 1 && wav_info.bits_per_sample == 16) {
        if ((length % sizeof(int16_t)) != 0) {
            ESP_LOGW(TAG, "Mono chunk size %zu not aligned to 16-bit samples - writing raw", length);
        } else {
            duplicate_to_stereo = true;
        }
    } else if (header_parsed && wav_info.num_channels == 1 && wav_info.bits_per_sample != 16) {
        ESP_LOGW(TAG, "Mono WAV with %u-bit samples not supported for duplication - writing raw",
                 (unsigned int)wav_info.bits_per_sample);
    }

    if (duplicate_to_stereo) {
        if (!ensure_stereo_scratch_buffer()) {
            ESP_LOGE(TAG, "Failed to provision stereo scratch buffer (%u bytes)", (unsigned int)CONFIG_TTS_STEREO_SCRATCH_BYTES);
            return ESP_ERR_NO_MEM;
        }

        const size_t sample_count = length / sizeof(int16_t);
        const int16_t *src = (const int16_t *)data;
        size_t processed_samples = 0;

        if (s_duplication_logs < 6) {
            ESP_LOGD(TAG, "[PCM DUP] %zu mono samples -> chunked stereo writes (scratch=%zu bytes)",
                     sample_count, s_stereo_scratch_size);
            s_duplication_logs++;
        }

        while (processed_samples < sample_count) {
            size_t remaining = sample_count - processed_samples;
            size_t block_samples = (remaining < s_stereo_scratch_capacity_samples) ? remaining : s_stereo_scratch_capacity_samples;
            int16_t *dst = (int16_t *)s_stereo_scratch;

            for (size_t i = 0; i < block_samples; ++i) {
                int16_t sample = src[processed_samples + i];
                size_t idx = i * 2;
                dst[idx] = sample;
                dst[idx + 1] = sample;
            }

            size_t block_bytes = block_samples * sizeof(int16_t) * 2U;
            size_t written = 0;
            esp_err_t ret = audio_driver_write((const uint8_t *)dst, block_bytes, &written, portMAX_DELAY);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Stereo duplication write failed mid-stream: %s", esp_err_to_name(ret));
                return ret;
            }
            if (written != block_bytes) {
                ESP_LOGW(TAG, "Stereo write partial: %zu/%zu bytes", written, block_bytes);
            }

            processed_samples += block_samples;
        }

        if (accounted_bytes) {
            *accounted_bytes = accounted;
        }
        return ESP_OK;
    }

    size_t written = 0;
    esp_err_t ret = audio_driver_write(data, length, &written, portMAX_DELAY);
    if (ret != ESP_OK) {
        return ret;
    }

    // Add comprehensive logging to verify audio playback
    static size_t total_bytes_played = 0;
    total_bytes_played += written;
    
    if (s_passthrough_logs < 6) {
        ESP_LOGI(TAG, "[PCM PLAYBACK] Successfully wrote %zu bytes to I2S driver (total: %zu bytes)", 
                 written, total_bytes_played);
        s_passthrough_logs++;
    } else if ((s_passthrough_logs % 100) == 0) {
        ESP_LOGI(TAG, "[PCM PLAYBACK] Ongoing - wrote %zu bytes (total: %zu bytes)", 
                 written, total_bytes_played);
    }
    s_passthrough_logs++;

    if (accounted_bytes) {
        *accounted_bytes = accounted;
    }
    return ESP_OK;
}

static inline uint16_t read_le16(const uint8_t *ptr) {
    return (uint16_t)(ptr[0] | (ptr[1] << 8));
}

static inline uint32_t read_le32(const uint8_t *ptr) {
    return (uint32_t)(ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24));
}

static esp_err_t parse_wav_header(const uint8_t *buffer, size_t length, size_t *header_consumed) {
    if (length < 12) {
        ESP_LOGD(TAG, "WAV header too short: %zu bytes (need at least 12)", length);
        return ESP_ERR_INVALID_SIZE;
    }

    // ✅ STREAMING FIX: Search for RIFF header within the buffer
    // In streaming scenarios, PCM data might arrive before the header due to network fragmentation
    // We need to find where the actual WAV header starts
    size_t riff_offset = 0;
    bool riff_found = false;
    
    if (memcmp(buffer, "RIFF", 4) == 0) {
        // Header is at the beginning (normal case)
        riff_found = true;
    } else {
        // Search for RIFF header within the accumulated buffer
        ESP_LOGD(TAG, "🔍 RIFF not at start, searching within %zu bytes...", length);
        for (size_t i = 0; i <= length - 4; i++) {
            if (memcmp(buffer + i, "RIFF", 4) == 0) {
                riff_offset = i;
                riff_found = true;
                ESP_LOGI(TAG, "✅ Found RIFF header at offset %zu (skipped %zu bytes of PCM data)", i, i);
                break;
            }
        }
    }
    
    if (!riff_found) {
        ESP_LOGD(TAG, "⏳ RIFF header not found in accumulated buffer (%zu bytes) - need more data", length);
        ESP_LOGD(TAG, "   First 4 bytes: 0x%02X 0x%02X 0x%02X 0x%02X", 
                 buffer[0], buffer[1], buffer[2], buffer[3]);
        return ESP_ERR_INVALID_ARG;
    }
    
    // Adjust buffer pointer and length to start from RIFF header
    const uint8_t *original_buffer = buffer;  // Save original pointer for offset calculation
    buffer = buffer + riff_offset;
    length = length - riff_offset;
    
    if (length < 12) {
        ESP_LOGD(TAG, "WAV header too short after offset adjustment: %zu bytes (need at least 12)", length);
        return ESP_ERR_INVALID_SIZE;
    }

    if (memcmp(buffer + 8, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "Invalid WAVE signature - got: 0x%02X 0x%02X 0x%02X 0x%02X at offset 8", 
                 buffer[8], buffer[9], buffer[10], buffer[11]);
        return ESP_ERR_INVALID_ARG;
    }

    size_t offset = 12;
    bool fmt_found = false;
    bool data_found = false;
    wav_runtime_info_t parsed = {0};

    while (offset + 8 <= length) {
        const uint8_t *chunk = buffer + offset;
        char chunk_id[5] = {0};
        memcpy(chunk_id, chunk, 4);
        uint32_t chunk_size = read_le32(chunk + 4);
        size_t chunk_data_start = offset + 8;

        if (chunk_data_start > length) {
            return ESP_ERR_INVALID_SIZE;
        }

        size_t remaining = length - chunk_data_start;

        if (memcmp(chunk_id, "fmt ", 4) == 0) {
            if (remaining < chunk_size) {
                return ESP_ERR_INVALID_SIZE;
            }

            if (chunk_size < 16) {
                ESP_LOGE(TAG, "fmt chunk too small: %lu", (unsigned long)chunk_size);
                return ESP_FAIL;
            }

            parsed.audio_format = read_le16(chunk + 8);
            parsed.num_channels = read_le16(chunk + 10);
            parsed.sample_rate = read_le32(chunk + 12);
            parsed.byte_rate = read_le32(chunk + 16);
            parsed.block_align = read_le16(chunk + 20);
            parsed.bits_per_sample = read_le16(chunk + 22);

            fmt_found = true;

        } else if (memcmp(chunk_id, "data", 4) == 0) {
            parsed.data_size = chunk_size;
            if (header_consumed != NULL) {
                // ✅ STREAMING FIX: Account for RIFF offset when calculating header_consumed
                // If RIFF was found at offset N, we need to skip those N bytes plus the header
                *header_consumed = riff_offset + chunk_data_start;
            }
            data_found = true;
            break;

        } else {
            if (remaining < chunk_size) {
                return ESP_ERR_INVALID_SIZE;
            }
        }

        size_t advance = chunk_size;
        offset = chunk_data_start + advance;
        if (advance & 1) {
            if (offset >= length) {
                return ESP_ERR_INVALID_SIZE;
            }
            offset += 1;
        }
    }

    if (!fmt_found) {
        ESP_LOGE(TAG, "fmt chunk missing in WAV header");
        return ESP_FAIL;
    }

    if (!data_found) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (parsed.audio_format != 1) {
        ESP_LOGE(TAG, "Unsupported audio format: %u (only PCM=1)", parsed.audio_format);
        return ESP_ERR_INVALID_ARG;
    }

    wav_info = parsed;
    ESP_LOGI(TAG, "✅ WAV header parsed successfully");
    return ESP_OK;
}

bool tts_decoder_has_pending_audio(void) {
    if (!is_running && !is_playing) {
        return false;
    }

    // ✅ FIX #5 (MOVED HERE): Check stream buffer but ignore remnant bytes < 100
    // Remnant bytes are smaller than minimum I2S DMA buffer size and will never play
    if (g_audio_stream_buffer != NULL) {
        size_t buffer_bytes = xStreamBufferBytesAvailable(g_audio_stream_buffer);
        if (buffer_bytes > 0 && buffer_bytes < 100) {
            // Remnant bytes - treat as "no pending audio"
            return false;
        }
        if (buffer_bytes >= 100) {
            return true;
        }
    }

    size_t received = bytes_received;
    size_t header_bytes = header_bytes_received;
    size_t played = pcm_bytes_played;

    // If we haven't parsed the header yet but have received data, we have pending audio
    if (!header_parsed) {
        return received > 0;
    }

    // If we have received data but haven't played enough, we have pending audio
    if (received <= header_bytes) {
        return false;
    }

    size_t payload_received = received - header_bytes;
    return played < payload_received;
}

size_t tts_decoder_get_pending_bytes(void) {
    size_t received = bytes_received;
    size_t header_bytes = header_bytes_received;
    size_t played = pcm_bytes_played;

    if (!header_parsed) {
        return received;
    }

    if (received <= header_bytes || played >= (received - header_bytes)) {
        return 0;
    }

    return (received - header_bytes) - played;
}

esp_err_t tts_decoder_wait_for_idle(uint32_t timeout_ms) {
    if (!is_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    TickType_t start_tick = xTaskGetTickCount();
    TickType_t timeout_ticks = (timeout_ms == 0) ? 0 : pdMS_TO_TICKS(timeout_ms);
    // ✅ FIX #14: Increased check interval from 20ms to 50ms to reduce false timeout warnings
    // Audio playback takes time - checking every 20ms was too aggressive
    const TickType_t sleep_ticks = pdMS_TO_TICKS(50);  // Was 20ms
    
    // More comprehensive check: track if we've actually started receiving audio
    bool had_audio_data = audio_data_received || bytes_received > 0;
    uint32_t wait_start_time = (uint32_t)(esp_timer_get_time() / 1000);
    
    // Track if we've ever started playing audio
    bool ever_playing = is_playing || playback_start_time > 0;
    
    // Variables for tracking state changes
    uint32_t state_change_count = 0;
    uint32_t identical_state_count = 0;
    uint32_t last_state_hash = 0;

    while (1) {
        // ✅ FIX #2: Check task handle FIRST - if NULL, playback task has exited cleanly
        // This is the PRIMARY indicator that the task completed its cleanup sequence
        if (g_playback_task_handle == NULL) {
            ESP_LOGI(TAG, "TTS playback task handle is NULL - task already exited (after %u checks)", 
                     (unsigned int)((xTaskGetTickCount() - start_tick) / sleep_ticks));
            return ESP_OK;
        }
        
        // ✅ FIX #2: Check playback_completed flag SECOND - this is set before task deletion
        // If this is true, the task is in the process of shutting down
        if (playback_completed) {
            ESP_LOGI(TAG, "TTS playback completed flag set - task exiting (after %u checks)", 
                     (unsigned int)((xTaskGetTickCount() - start_tick) / sleep_ticks));
            return ESP_OK;
        }
        
        // Check if decoder is no longer running - also indicates completion
        if (!is_running) {
            ESP_LOGI(TAG, "TTS decoder not running - idle (after %u checks)", 
                     (unsigned int)((xTaskGetTickCount() - start_tick) / sleep_ticks));
            return ESP_OK;
        }
        
        // Check if we're truly idle (not playing and no pending audio)
        bool has_pending = tts_decoder_has_pending_audio();
        size_t pending_bytes = tts_decoder_get_pending_bytes();
        bool still_playing = is_playing;
        bool playback_done = playback_completed;
        
        // ✅ FIX #4: Treat small remnant bytes (< 100) as "not pending"
        // These are often incomplete audio frames that will never play out
        // This commonly happens when the last chunk is smaller than I2S DMA buffer size
        if (has_pending && pending_bytes > 0 && pending_bytes < 100) {
            ESP_LOGD(TAG, "Ignoring %zu remnant bytes (too small for I2S playback)", pending_bytes);
            has_pending = false; // Treat as no pending audio
        }
        
        // CRITICAL IMPROVEMENT: Create state hash for detecting stuck conditions
        uint32_t current_state_hash = (has_pending ? 1 : 0) | 
                                      (still_playing ? 2 : 0) | 
                                      (playback_done ? 4 : 0);
        
        // Check if state has changed
        if (current_state_hash != last_state_hash) {
            state_change_count++;
            identical_state_count = 0; // Reset identical state counter on state change
            last_state_hash = current_state_hash;
            ESP_LOGD(TAG, "TTS state changed: has_pending=%d, is_playing=%d, completed=%d (changes: %u)", 
                     (int)has_pending, (int)still_playing, (int)playback_done, (unsigned int)state_change_count);
        } else {
            identical_state_count++;
            
            // CRITICAL IMPROVEMENT: If stuck in same state for too long, force completion
            if (identical_state_count > 200) { // About 4 seconds in same state
                ESP_LOGW(TAG, "TTS stuck in same state for too long (%u checks) - forcing completion", 
                         (unsigned int)identical_state_count);
                // Try to flush and reset before forcing stop
                esp_err_t flush_ret = tts_decoder_flush_and_reset();
                if (flush_ret != ESP_OK) {
                    ESP_LOGW(TAG, "TTS flush and reset failed: %s - forcing stop", esp_err_to_name(flush_ret));
                    tts_decoder_stop();
                }
                break;
            }
        }
        
        // Update state tracking (removed unused static variables)
        
        // CRITICAL IMPROVEMENT: Better detection of idle state
        // If we never received audio and never played, we're idle
        if (!had_audio_data && !ever_playing && bytes_received == 0 && !still_playing) {
            ESP_LOGD(TAG, "TTS truly idle - no audio ever received (after %u checks)", 
                     (unsigned int)((xTaskGetTickCount() - start_tick) / sleep_ticks));
            return ESP_OK;
        }
        
        // If we received audio but playback is done and no pending audio, we're idle
        if (had_audio_data && playback_done && !has_pending && !still_playing) {
            ESP_LOGD(TAG, "TTS idle - audio processed and playback completed (after %u checks)", 
                     (unsigned int)((xTaskGetTickCount() - start_tick) / sleep_ticks));
            return ESP_OK;
        }
        
        // If EOS was requested and no pending audio, we can consider ourselves idle
        if (eos_requested && !has_pending && !still_playing) {
            ESP_LOGD(TAG, "TTS idle - EOS requested and no pending audio (after %u checks)", 
                     (unsigned int)((xTaskGetTickCount() - start_tick) / sleep_ticks));
            return ESP_OK;
        }
        
        // ✅ FIX #10: If no pending audio but still marked as playing, the task is completing
        // This happens when the playback task is in its final iteration draining the I2S buffer
        // After a few checks (to avoid false positives), treat this as idle
        if (!has_pending && still_playing && identical_state_count > 5) {
            ESP_LOGI(TAG, "TTS idle - no pending audio, playback completing (state stable for %u checks)", 
                     (unsigned int)identical_state_count);
            return ESP_OK;
        }
        
        // Check for timeout
        if (timeout_ms > 0 && (xTaskGetTickCount() - start_tick) >= timeout_ticks) {
            // CRITICAL IMPROVEMENT: More informative timeout logging
            size_t pending_bytes = tts_decoder_get_pending_bytes();
            size_t received_bytes = bytes_received;
            size_t played_bytes = pcm_bytes_played;
            size_t header_bytes = header_bytes_received;
            
            ESP_LOGW(TAG, "TTS wait for idle timeout after %u ms", (unsigned int)timeout_ms);
            ESP_LOGW(TAG, "  State: has_pending=%d, is_playing=%d, completed=%d, eos_requested=%d", 
                     (int)has_pending, (int)still_playing, (int)playback_done, (int)eos_requested);
            ESP_LOGW(TAG, "  Data: received=%zu, header=%zu, played=%zu, pending=%zu", 
                     (unsigned int)received_bytes, (unsigned int)header_bytes, 
                     (unsigned int)played_bytes, (unsigned int)pending_bytes);
            return ESP_ERR_TIMEOUT;
        }
        
        // Reset watchdog to prevent system reset during long waits
        safe_task_wdt_reset();
        vTaskDelay(sleep_ticks);
        
        // Update had_audio_data flag if we receive audio or bytes
        if ((audio_data_received || bytes_received > 0) && !had_audio_data) {
            had_audio_data = true;
            wait_start_time = (uint32_t)(esp_timer_get_time() / 1000);
            ESP_LOGD(TAG, "TTS received first audio data - updating wait start time");
        }
        
        // Update ever_playing flag
        if (is_playing && !ever_playing) {
            ever_playing = true;
            ESP_LOGD(TAG, "TTS started playing audio for the first time");
        }
        
        // Additional safeguard: if we've been waiting for a long time and haven't completed playback
        uint32_t current_time = (uint32_t)(esp_timer_get_time() / 1000);
        if (had_audio_data && (current_time - wait_start_time) > 15000) { // 15 seconds (increased from 10)
            ESP_LOGW(TAG, "Long wait for TTS idle (%u seconds) - forcing completion", 
                     (unsigned int)(current_time - wait_start_time) / 1000);
            
            // CRITICAL IMPROVEMENT: Instead of forcing stop, try to flush and reset first
            // This prevents audio cut-offs and lets the system handle it gracefully
            esp_err_t flush_ret = tts_decoder_flush_and_reset();
            if (flush_ret != ESP_OK) {
                ESP_LOGW(TAG, "TTS flush and reset on long wait failed: %s - forcing stop", esp_err_to_name(flush_ret));
                // Force stop to ensure we don't hang indefinitely
                tts_decoder_stop();
            }
            break;
        }
    }

    ESP_LOGI(TAG, "TTS drain complete - pipeline finished after %u checks", 
             (unsigned int)((xTaskGetTickCount() - start_tick) / sleep_ticks));
    return ESP_OK;
}

void tts_decoder_reset_session(void) {
    // Reset session-specific state while keeping decoder running
    header_parsed = false;
    header_bytes_received = 0;
    bytes_received = 0;
    pcm_bytes_played = 0;
    playback_feedback_sent = false;
    eos_requested = false;
    playback_completed = false;
    audio_data_received = false;
    playback_start_time = (uint32_t)(esp_timer_get_time() / 1000);
    is_session_active = false;
    session_start_time = 0;
    force_stop_requested = false;
    memset(&wav_info, 0, sizeof(wav_info));
    
    // Clear the stream buffer
    if (g_audio_stream_buffer != NULL) {
        size_t buffer_level = xStreamBufferBytesAvailable(g_audio_stream_buffer);
        if (buffer_level > 0) {
            ESP_LOGI(TAG, "Clearing %zu bytes from stream buffer during session reset", buffer_level);
        }
        xStreamBufferReset(g_audio_stream_buffer);
    }
    
    ESP_LOGI(TAG, "TTS decoder session reset for next audio stream");
}

esp_err_t tts_decoder_flush_and_reset(void) {
    ESP_LOGI(TAG, "🔄 Flushing and resetting TTS decoder for session transition");
    
    // First make sure any pending audio is processed
    if (tts_decoder_has_pending_audio() || is_playing) {
        ESP_LOGI(TAG, "Flushing pending audio before reset (~%zu bytes)", tts_decoder_get_pending_bytes());
        
        // ✅ FIX #11: Increase timeout from 3000ms to 5000ms for longer TTS responses
        // At 16kHz sample rate, 11000 bytes = ~0.7 seconds of audio
        // Need extra time for I2S buffer draining and task cleanup
        esp_err_t ret = tts_decoder_wait_for_idle(5000); // 5 second timeout for flushing
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "TTS flush timeout during reset - forcing stop");
            // Force stop if waiting didn't work
            if (is_running) {
                tts_decoder_stop();
            }
        }
    } else {
        ESP_LOGI(TAG, "No pending audio to flush");
    }
    
    // Now reset the session to ensure clean state for next session
    tts_decoder_reset_session();
    is_playing = false;
    is_running = false;
    
    ESP_LOGI(TAG, "✅ TTS decoder flushed and reset for next session");
    return ESP_OK;
}

static void print_wav_info(const wav_runtime_info_t *info) {
    ESP_LOGI(TAG, "=== WAV File Info ===");
    ESP_LOGI(TAG, "Sample Rate: %lu Hz", (unsigned long)info->sample_rate);
    ESP_LOGI(TAG, "Channels: %u", (unsigned int)info->num_channels);
    ESP_LOGI(TAG, "Bits per Sample: %u", (unsigned int)info->bits_per_sample);
    ESP_LOGI(TAG, "Audio Format: %u (PCM)", (unsigned int)info->audio_format);
    ESP_LOGI(TAG, "Declared Data Size: %lu bytes", (unsigned long)info->data_size);
    ESP_LOGI(TAG, "Block Align: %u", (unsigned int)info->block_align);
    ESP_LOGI(TAG, "Byte Rate: %lu", (unsigned long)info->byte_rate);
    ESP_LOGI(TAG, "====================");
}

static bool ensure_stereo_scratch_buffer(void) {
    if (s_stereo_scratch != NULL) {
        return true;
    }

    // CRITICAL FIX: Always allocate stereo scratch buffer in PSRAM to prevent internal DMA fragmentation
    // Internal DMA-capable RAM is only ~60KB and gets severely fragmented when this 8KB buffer is allocated
    // PSRAM has 4MB available and doesn't suffer from the same fragmentation issues
    uint8_t *buf = heap_caps_aligned_alloc(4, CONFIG_TTS_STEREO_SCRATCH_BYTES, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        // Fallback to internal DMA only if PSRAM allocation fails (shouldn't happen with 4MB PSRAM)
        ESP_LOGW(TAG, "PSRAM allocation failed - attempting internal DMA (may cause fragmentation)");
        buf = heap_caps_aligned_alloc(4, CONFIG_TTS_STEREO_SCRATCH_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    }

    if (buf == NULL) {
        ESP_LOGE(TAG, "Unable to allocate stereo duplication scratch buffer (%u bytes)", (unsigned int)CONFIG_TTS_STEREO_SCRATCH_BYTES);
        return false;
    }

    s_stereo_scratch = buf;
    s_stereo_scratch_size = CONFIG_TTS_STEREO_SCRATCH_BYTES;
    s_stereo_scratch_capacity_samples = s_stereo_scratch_size / (sizeof(int16_t) * 2U);

    if (s_stereo_scratch_capacity_samples == 0) {
        ESP_LOGE(TAG, "Stereo scratch buffer too small (%zu bytes) for duplication", s_stereo_scratch_size);
        heap_caps_free(s_stereo_scratch);
        s_stereo_scratch = NULL;
        s_stereo_scratch_size = 0;
        return false;
    }

    ESP_LOGI(TAG, "[PCM DUP] Scratch buffer ready: %zu bytes (%zu samples per block) in PSRAM",
             s_stereo_scratch_size, s_stereo_scratch_capacity_samples);
    return true;
}

void tts_decoder_notify_end_of_stream(void) {
    if (!is_running) {
        return;
    }

    // Mark that EOS has been requested
    eos_requested = true;
    audio_data_received = false; // Reset audio data flag for next session
    playback_completed = false;  // Reset completion flag for next session
    
    ESP_LOGI(TAG, "TTS end-of-stream signaled (bytes_received=%zu, header_parsed=%d)", 
             (unsigned int)bytes_received, (int)header_parsed);
    
    // Reset session-specific state for the next audio session
    header_parsed = false;
    header_bytes_received = 0;
    bytes_received = 0;
    pcm_bytes_played = 0;
    playback_feedback_sent = false;
    playback_completed = false;
    is_session_active = false;
    session_start_time = 0;
    force_stop_requested = false;
    memset(&wav_info, 0, sizeof(wav_info));
    
    // More aggressive flushing if we have accumulated data
    if (g_audio_stream_buffer != NULL) {
        size_t buffer_level = xStreamBufferBytesAvailable(g_audio_stream_buffer);
        if (buffer_level > 0) {
            ESP_LOGI(TAG, "Flushing %zu bytes from stream buffer", (unsigned int)buffer_level);
            
            // Give the playback task a chance to process the data first
            vTaskDelay(pdMS_TO_TICKS(100)); // Increased from 50ms to 100ms
            
            // If still not processed, force drain
            buffer_level = xStreamBufferBytesAvailable(g_audio_stream_buffer);
            if (buffer_level > 0) {
                ESP_LOGI(TAG, "Force draining %zu bytes from stream buffer", (unsigned int)buffer_level);
                uint8_t dummy_buffer[1024];
                size_t bytes_drained = 0;
                uint32_t drain_attempts = 0;
                const uint32_t max_drain_attempts = 100; // Increased from 50 to 100 * 10ms = 1000ms max
                
                while (xStreamBufferBytesAvailable(g_audio_stream_buffer) > 0 && drain_attempts < max_drain_attempts) {
                    size_t chunk = xStreamBufferReceive(g_audio_stream_buffer, 
                                                       dummy_buffer, 
                                                       sizeof(dummy_buffer), 
                                                       pdMS_TO_TICKS(10));
                    if (chunk > 0) {
                        bytes_drained += chunk;
                    } else {
                        drain_attempts++;
                    }
                    vTaskDelay(pdMS_TO_TICKS(10));
                    safe_task_wdt_reset(); // Reset watchdog during draining
                }
                ESP_LOGI(TAG, "Flushed %zu bytes from stream buffer (attempts: %u)", 
                         (unsigned int)bytes_drained, (unsigned int)drain_attempts);
            }
        } else {
            ESP_LOGD(TAG, "Stream buffer empty - no flush needed");
        }
    }
    
    // Ensure playback task knows about EOS
    if (g_playback_task_handle != NULL) {
        // Notify task about EOS
        xTaskNotifyGive(g_playback_task_handle);
    }
    
    // Wait for playback to complete before returning with better timeout handling
    const TickType_t wait_start = xTaskGetTickCount();
    const TickType_t wait_timeout = pdMS_TO_TICKS(1000); // Increased from 500ms to 1000ms timeout
    
    while ((xTaskGetTickCount() - wait_start) < wait_timeout) {
        if (playback_completed || !is_playing) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        safe_task_wdt_reset(); // Reset watchdog during waiting
    }
    
    if (!playback_completed && is_playing) {
        ESP_LOGW(TAG, "TTS playback did not complete within timeout - forcing completion");
        // Try to stop the decoder gracefully
        esp_err_t stop_ret = tts_decoder_stop();
        if (stop_ret != ESP_OK) {
            ESP_LOGW(TAG, "TTS decoder stop failed: %s", esp_err_to_name(stop_ret));
        }
    }
    
    // Reset the playback start time for next session
    playback_start_time = (uint32_t)(esp_timer_get_time() / 1000);
    
    // Additional improvements for sequential session handling
    // Clear the stream buffer to prevent data carryover between sessions
    if (g_audio_stream_buffer != NULL) {
        xStreamBufferReset(g_audio_stream_buffer);
        ESP_LOGD(TAG, "Stream buffer reset for next session");
    }
    
    // Reset all counters for the next session
    bytes_received = 0;
    pcm_bytes_played = 0;
    header_bytes_received = 0;
    header_parsed = false;
    audio_data_received = false;
    playback_feedback_sent = false;
    playback_completed = false;
    eos_requested = false;
    is_session_active = false;
    session_start_time = 0;
    force_stop_requested = false;
    
    ESP_LOGI(TAG, "TTS decoder session reset for next audio stream");
}

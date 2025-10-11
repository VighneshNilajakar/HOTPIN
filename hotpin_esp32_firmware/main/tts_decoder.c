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
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>

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

// Audio buffer queue
#define AUDIO_QUEUE_SIZE        10
#define AUDIO_CHUNK_SIZE        4096

typedef struct {
    uint8_t *data;
    size_t length;
} audio_chunk_t;

// State management
static bool is_initialized = false;
static volatile bool is_playing = false;
static volatile bool is_running = false;
static volatile bool header_parsed = false;
static volatile bool playback_feedback_sent = false;
static volatile bool eos_requested = false;

// WAV header info
static wav_runtime_info_t wav_info;
static volatile size_t bytes_received = 0;
static volatile size_t pcm_bytes_played = 0;

// Playback queue and task
static QueueHandle_t g_audio_queue = NULL;
static TaskHandle_t g_playback_task_handle = NULL;

#define WAV_HEADER_BUFFER_MAX   8192

// Buffer for header accumulation
static uint8_t header_buffer[WAV_HEADER_BUFFER_MAX] __attribute__((aligned(4)));
static volatile size_t header_bytes_received = 0;
static volatile bool eos_signal_queued = false;

// ===========================
// Private Function Declarations
// ===========================
static void tts_playback_task(void *pvParameters);
static esp_err_t parse_wav_header(const uint8_t *buffer, size_t length, size_t *header_consumed);
static void print_wav_info(const wav_runtime_info_t *info);
static void audio_data_callback(const uint8_t *data, size_t len, void *arg);

// ===========================
// Public Functions
// ===========================

esp_err_t tts_decoder_init(void) {
    ESP_LOGI(TAG, "Initializing TTS decoder...");
    
    if (is_initialized) {
        ESP_LOGW(TAG, "TTS decoder already initialized");
        return ESP_OK;
    }
    
    // Create audio queue
    g_audio_queue = xQueueCreate(AUDIO_QUEUE_SIZE, sizeof(audio_chunk_t));
    if (g_audio_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create audio queue");
        return ESP_ERR_NO_MEM;
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
    
    // Delete queue
    if (g_audio_queue != NULL) {
        // Clear any remaining items
        audio_chunk_t chunk;
        while (xQueueReceive(g_audio_queue, &chunk, 0) == pdTRUE) {
            if (chunk.data != NULL) {
                heap_caps_free(chunk.data);
            }
        }
        vQueueDelete(g_audio_queue);
        g_audio_queue = NULL;
    }
    
    is_initialized = false;
    ESP_LOGI(TAG, "TTS decoder deinitialized");
    
    return ESP_OK;
}

esp_err_t tts_decoder_start(void) {
    ESP_LOGI(TAG, "Starting TTS decoder...");
    
    if (!is_initialized) {
        ESP_LOGE(TAG, "TTS decoder not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (is_running) {
        ESP_LOGW(TAG, "TTS decoder already running");
        return ESP_OK;
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
    eos_signal_queued = false;
    playback_feedback_sent = false;
    eos_requested = false;
    memset(&wav_info, 0, sizeof(wav_info));
    
    // CRITICAL FIX: Pin TTS playback task to Core 0 (same as Wi-Fi and STT tasks)
    // Co-locating all high-bandwidth audio tasks on Core 0 resolves hardware bus contention
    ESP_LOGI(TAG, "[CORE AFFINITY] Creating TTS playback task on Core 0 (co-located with Wi-Fi and STT)");
    BaseType_t ret = xTaskCreatePinnedToCore(
        tts_playback_task,
        "tts_playback",
        TASK_STACK_SIZE_MEDIUM,
        NULL,
        TASK_PRIORITY_TTS_DECODER,
        &g_playback_task_handle,
        0  // Core 0 - CRITICAL: Co-locate with Wi-Fi and I2S RX to prevent DMA corruption
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create playback task");
        return ESP_FAIL;
    }
    
    esp_err_t clk_ret = audio_driver_set_tx_sample_rate(CONFIG_AUDIO_SAMPLE_RATE);
    if (clk_ret != ESP_OK) {
        ESP_LOGW(TAG, "Unable to reset TX sample rate at decoder start: %s", esp_err_to_name(clk_ret));
    }

    is_running = true;
    is_playing = true;
    
    ESP_LOGI(TAG, "✅ TTS decoder started");
    return ESP_OK;
}

esp_err_t tts_decoder_stop(void) {
    ESP_LOGI(TAG, "Stopping TTS decoder...");
    
    if (!is_running) {
        ESP_LOGW(TAG, "TTS decoder not running");
        return ESP_OK;
    }
    
    is_playing = false;
    is_running = false;
    eos_requested = false;

    // Notify playback task to exit quickly
    if (g_audio_queue != NULL && !eos_signal_queued) {
        audio_chunk_t stop_signal = {
            .data = NULL,
            .length = 0
        };

        audio_chunk_t dropped;
        for (int attempts = 0; attempts < 3; attempts++) {
            if (xQueueSend(g_audio_queue, &stop_signal, pdMS_TO_TICKS(10)) == pdTRUE) {
                eos_signal_queued = true;
                break;
            }
            if (xQueueReceive(g_audio_queue, &dropped, 0) == pdTRUE) {
                if (dropped.data != NULL) {
                    heap_caps_free(dropped.data);
                }
            }
        }

        if (!eos_signal_queued) {
            ESP_LOGW(TAG, "Unable to queue playback stop sentinel during shutdown");
        }
    }

    // Wait for playback task to cleanly terminate
    const TickType_t wait_step = pdMS_TO_TICKS(10);
    for (int attempts = 0; attempts < 50; attempts++) {
        if (g_playback_task_handle == NULL) {
            break;
        }
        vTaskDelay(wait_step);
    }

    // Force delete if still running (should be rare)
    if (g_playback_task_handle != NULL) {
        vTaskDelete(g_playback_task_handle);
        g_playback_task_handle = NULL;
    }

    esp_err_t clk_ret = audio_driver_set_tx_sample_rate(CONFIG_AUDIO_SAMPLE_RATE);
    if (clk_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to restore TX sample rate during stop: %s", esp_err_to_name(clk_ret));
    }
    
    ESP_LOGI(TAG, "TTS decoder stopped (played %zu bytes)", pcm_bytes_played);
    return ESP_OK;
}

bool tts_decoder_is_playing(void) {
    return is_playing;
}

// ===========================
// Private Functions
// ===========================

static void audio_data_callback(const uint8_t *data, size_t len, void *arg) {
    ESP_LOGD(TAG, "Received audio chunk: %zu bytes", len);

    if (!is_running) {
        ESP_LOGW(TAG, "Decoder not running - dropping %zu-byte chunk", len);
        return;
    }
    
    // Allocate memory for chunk
    uint8_t *chunk_data = heap_caps_aligned_alloc(4, len, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (chunk_data == NULL) {
        ESP_LOGW(TAG, "DMA-capable alloc failed, attempting fallback for %zu-byte chunk", len);
        chunk_data = heap_caps_malloc(len, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    }
    if (chunk_data == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for audio chunk (%zu bytes)", len);
        return;
    }
    
    memcpy(chunk_data, data, len);
    
    // Create chunk descriptor
    audio_chunk_t chunk = {
        .data = chunk_data,
        .length = len
    };
    
    // Send to playback queue
    const TickType_t enqueue_timeout = pdMS_TO_TICKS(1000);
    if (xQueueSend(g_audio_queue, &chunk, enqueue_timeout) != pdTRUE) {
        UBaseType_t queued = uxQueueMessagesWaiting(g_audio_queue);
        ESP_LOGW(TAG, "Audio queue full (depth=%u) after %lu ms wait - dropping %zu-byte chunk",
                 (unsigned int)queued, (unsigned long)(enqueue_timeout * portTICK_PERIOD_MS), len);
        heap_caps_free(chunk_data);
    } else {
        bytes_received += len;
        ESP_LOGD(TAG, "Queued audio chunk (%zu bytes, total received: %zu)", len, bytes_received);
    }
}

static void tts_playback_task(void *pvParameters) {
    ESP_LOGI(TAG, "TTS playback task started on Core %d", xPortGetCoreID());
    
    audio_chunk_t chunk;
    
    while (true) {
        // Wait for audio chunk
        if (xQueueReceive(g_audio_queue, &chunk, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (chunk.data == NULL) {
                ESP_LOGI(TAG, "Playback stop signal received");
                eos_requested = false;
                break;
            }

            // Parse header if not done yet
            if (!header_parsed) {
                if (header_bytes_received + chunk.length > WAV_HEADER_BUFFER_MAX) {
                    ESP_LOGE(TAG, "Header staging buffer overflow (%zu + %zu)",
                             header_bytes_received, chunk.length);
                    is_running = false;
                } else {
                    memcpy(header_buffer + header_bytes_received, chunk.data, chunk.length);
                    header_bytes_received += chunk.length;

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

                        size_t pcm_len = header_bytes_received - header_consumed;
                        if (pcm_len > 0) {
                            size_t bytes_written = 0;
                            esp_err_t write_ret = audio_driver_write(header_buffer + header_consumed,
                                                                     pcm_len,
                                                                     &bytes_written,
                                                                     portMAX_DELAY);
                            if (write_ret == ESP_OK) {
                                pcm_bytes_played += bytes_written;
                                ESP_LOGD(TAG, "Played %zu bytes from initial chunk (total: %zu)",
                                         bytes_written, pcm_bytes_played);
                            } else {
                                ESP_LOGE(TAG, "Initial PCM write failed: %s", esp_err_to_name(write_ret));
                            }
                        }

                        header_bytes_received = 0;  // Reset buffer for future sessions
                    } else if (ret == ESP_ERR_INVALID_SIZE) {
                        ESP_LOGD(TAG, "Awaiting more header bytes (%zu collected)", header_bytes_received);
                    } else {
                        ESP_LOGE(TAG, "Failed to parse WAV header: %s", esp_err_to_name(ret));
                        is_running = false;
                    }
                }

                heap_caps_free(chunk.data);
                continue;

            } else {
                // Header already parsed - play PCM data directly
                if (!playback_feedback_sent) {
                    esp_err_t fb_ret = audio_feedback_beep_single(false);
                    if (fb_ret != ESP_OK) {
                        ESP_LOGW(TAG, "Delayed playback feedback failed: %s", esp_err_to_name(fb_ret));
                    } else {
                        ESP_LOGI(TAG, "🔔 Playback start feedback dispatched (late)");
                    }
                    playback_feedback_sent = true;
                }

                size_t bytes_written = 0;
                esp_err_t ret = audio_driver_write(chunk.data, chunk.length, 
                                                     &bytes_written, portMAX_DELAY);
                
                if (ret == ESP_OK) {
                    pcm_bytes_played += bytes_written;
                    UBaseType_t queued = (g_audio_queue != NULL) ? uxQueueMessagesWaiting(g_audio_queue) : 0;
                    ESP_LOGD(TAG, "Played %zu bytes (total: %zu, queue depth: %u)",
                             bytes_written, pcm_bytes_played, (unsigned int)queued);
                } else {
                    ESP_LOGE(TAG, "Audio playback error: %s", esp_err_to_name(ret));
                }
            }
            
            // Free chunk memory
            heap_caps_free(chunk.data);

            if (eos_requested && g_audio_queue != NULL && uxQueueMessagesWaiting(g_audio_queue) == 0) {
                ESP_LOGI(TAG, "EOS drain complete - terminating playback task");
                eos_requested = false;
                break;
            }
        }
        else {
            if (eos_requested && g_audio_queue != NULL && uxQueueMessagesWaiting(g_audio_queue) == 0) {
                ESP_LOGI(TAG, "EOS idle timeout - terminating playback task");
                eos_requested = false;
                break;
            }

            if (!is_running && g_audio_queue != NULL && uxQueueMessagesWaiting(g_audio_queue) == 0) {
                break;
            }
        }
    }
    
    // Flush any remaining queued chunks
    while (xQueueReceive(g_audio_queue, &chunk, 0) == pdTRUE) {
        if (chunk.data != NULL) {
            heap_caps_free(chunk.data);
        }
    }

    is_running = false;
    is_playing = false;
    eos_signal_queued = false;

    if (pcm_bytes_played > 0) {
        esp_err_t clk_ret = audio_driver_set_tx_sample_rate(CONFIG_AUDIO_SAMPLE_RATE);
        if (clk_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to restore TX sample rate to %u Hz: %s",
                     (unsigned int)CONFIG_AUDIO_SAMPLE_RATE, esp_err_to_name(clk_ret));
        }

        esp_err_t fb_ret = audio_feedback_beep_double(false);
        if (fb_ret != ESP_OK) {
            ESP_LOGW(TAG, "Playback completion feedback failed: %s", esp_err_to_name(fb_ret));
        } else {
            ESP_LOGI(TAG, "🔔 Playback completion feedback dispatched (total bytes: %zu)", pcm_bytes_played);
        }
    }

    ESP_LOGI(TAG, "TTS playback task stopped");
    if (g_playback_task_handle == xTaskGetCurrentTaskHandle()) {
        g_playback_task_handle = NULL;
    }
    vTaskDelete(NULL);
}

static inline uint16_t read_le16(const uint8_t *ptr) {
    return (uint16_t)(ptr[0] | (ptr[1] << 8));
}

static inline uint32_t read_le32(const uint8_t *ptr) {
    return (uint32_t)(ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24));
}

static esp_err_t parse_wav_header(const uint8_t *buffer, size_t length, size_t *header_consumed) {
    if (length < 12) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (memcmp(buffer, "RIFF", 4) != 0) {
        ESP_LOGE(TAG, "Invalid RIFF chunk");
        return ESP_ERR_INVALID_ARG;
    }

    if (memcmp(buffer + 8, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "Invalid WAVE signature");
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
                *header_consumed = chunk_data_start;
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

    if (g_audio_queue != NULL && uxQueueMessagesWaiting(g_audio_queue) > 0) {
        return true;
    }

    size_t received = bytes_received;
    size_t header_bytes = header_bytes_received;
    size_t played = pcm_bytes_played;

    if (!header_parsed) {
        return received > 0;
    }

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
    const TickType_t sleep_ticks = pdMS_TO_TICKS(20);

    while (tts_decoder_has_pending_audio() || is_playing) {
        if (timeout_ms > 0 && (xTaskGetTickCount() - start_tick) >= timeout_ticks) {
            return ESP_ERR_TIMEOUT;
        }
        esp_task_wdt_reset();
        vTaskDelay(sleep_ticks);
    }

    return ESP_OK;
}

void tts_decoder_notify_end_of_stream(void) {
    if (!is_running || g_audio_queue == NULL) {
        return;
    }

    if (eos_signal_queued) {
        ESP_LOGD(TAG, "EOS sentinel already queued");
        return;
    }

    audio_chunk_t stop_signal = {
        .data = NULL,
        .length = 0
    };

    eos_requested = true;

    if (xQueueSend(g_audio_queue, &stop_signal, pdMS_TO_TICKS(500)) == pdTRUE) {
        eos_signal_queued = true;
        ESP_LOGI(TAG, "Queued TTS playback stop sentinel (server EOS)");
    } else {
        UBaseType_t depth = g_audio_queue ? uxQueueMessagesWaiting(g_audio_queue) : 0;
        ESP_LOGW(TAG, "Failed to queue EOS sentinel (queue depth=%u) - waiting for natural drain", (unsigned int)depth);
    }
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

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
#define TTS_STREAM_BUFFER_SIZE (16 * 1024)  // 16KB buffer for incoming audio
#define TTS_STREAM_BUFFER_TRIGGER_LEVEL 1   // Trigger on any data
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
        ESP_LOGI(TAG, "Allocating %d byte PSRAM buffer for TTS stream", TTS_STREAM_BUFFER_SIZE);
        g_stream_buffer_storage = (uint8_t *)heap_caps_malloc(TTS_STREAM_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
        if (g_stream_buffer_storage == NULL) {
            ESP_LOGE(TAG, "Failed to allocate PSRAM for stream buffer storage");
            return ESP_ERR_NO_MEM;
        }

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
    playback_feedback_sent = false;
    eos_requested = false;
    memset(&wav_info, 0, sizeof(wav_info));
    
    // CRITICAL FIX: Move TTS playback task to Core 1 to prevent Core 0 starvation
    // Core 0 handles WiFi/TCP and STT input, Core 1 handles state management and TTS output
    ESP_LOGI(TAG, "[CORE AFFINITY] Creating TTS playback task on Core 1 (APP_CPU)");
    BaseType_t ret = xTaskCreatePinnedToCore(
        tts_playback_task,
        "tts_playback",
        TASK_STACK_SIZE_MEDIUM,
        NULL,
        TASK_PRIORITY_TTS_DECODER,
        &g_playback_task_handle,
        TASK_CORE_CONTROL  // Core 1 - Balance load across cores to prevent watchdog timeout
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
    
    system_event_t evt = {
        .type = SYSTEM_EVENT_TTS_PLAYBACK_STARTED,
        .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL),
    };
    if (!event_dispatcher_post(&evt, pdMS_TO_TICKS(10))) {
        ESP_LOGW(TAG, "Failed to enqueue TTS playback start event");
    }

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
    eos_requested = true;

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

    if (g_audio_stream_buffer != NULL) {
        size_t bytes_sent = xStreamBufferSend(g_audio_stream_buffer, data, len, pdMS_TO_TICKS(100));
        if (bytes_sent != len) {
            ESP_LOGW(TAG, "Stream buffer full or timeout - dropped %d bytes", len - bytes_sent);
        } else {
            bytes_received += len;
            ESP_LOGD(TAG, "Sent %zu bytes to stream buffer (total received: %zu)", len, bytes_received);
        }
    }
}

static void tts_playback_task(void *pvParameters) {
    // ESP_LOGI(TAG, "TTS playback task started on Core %d", xPortGetCoreID());

    uint8_t dma_buffer[AUDIO_CHUNK_SIZE];
    esp_err_t playback_result = ESP_OK;

    while (is_running) {
        // Wait for data in the stream buffer
        size_t bytes_received_from_stream = xStreamBufferReceive(
            g_audio_stream_buffer,
            dma_buffer,
            sizeof(dma_buffer),
            pdMS_TO_TICKS(100) // Wait up to 100ms for data
        );

        if (bytes_received_from_stream > 0) {
            // We have data, process it
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
                        size_t accounted = 0;
                        esp_err_t write_ret = write_pcm_chunk_to_driver(header_buffer + header_consumed,
                                                                       pcm_len,
                                                                       &accounted);
                        if (write_ret == ESP_OK) {
                            pcm_bytes_played += accounted;
                            ESP_LOGD(TAG, "Played %zu bytes from initial chunk (total: %zu)",
                                     accounted, pcm_bytes_played);
                        } else {
                            ESP_LOGE(TAG, "Initial PCM write failed: %s", esp_err_to_name(write_ret));
                            playback_result = write_ret;
                            is_running = false;
                            break;
                        }
                    }

                    header_bytes_received = 0;  // Reset buffer for future sessions
                } else if (ret == ESP_ERR_INVALID_SIZE) {
                    ESP_LOGD(TAG, "Awaiting more header bytes (%zu collected)", header_bytes_received);
                } else {
                    ESP_LOGE(TAG, "Failed to parse WAV header: %s", esp_err_to_name(ret));
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
                    ESP_LOGD(TAG, "Played %zu bytes (total: %zu)", accounted, pcm_bytes_played);
                } else {
                    ESP_LOGE(TAG, "Audio playback error: %s", esp_err_to_name(ret));
                    playback_result = ret;
                    is_running = false;
                    break;
                }
            }
        } else {
            // Timeout occurred - check if we should exit
            if (eos_requested && xStreamBufferIsEmpty(g_audio_stream_buffer)) {
                ESP_LOGI(TAG, "EOS requested and stream buffer is empty. Exiting playback task.");
                eos_requested = false;
                break;
            }
        }
    }
    
    ESP_LOGI(TAG, "TTS playback task exiting (played %zu bytes)", pcm_bytes_played);
    is_running = false;
    g_playback_task_handle = NULL;
    vTaskDelete(NULL);
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

    if (s_passthrough_logs < 6) {
        ESP_LOGD(TAG, "[PCM PASS] Wrote %zu raw bytes", length);
        s_passthrough_logs++;
    }

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

    if (g_audio_stream_buffer != NULL && xStreamBufferBytesAvailable(g_audio_stream_buffer) > 0) {
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
    if (!is_running) {
        return;
    }

    eos_requested = true;
    ESP_LOGI(TAG, "TTS end-of-stream signaled");
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

    uint8_t *buf = heap_caps_aligned_alloc(4, CONFIG_TTS_STEREO_SCRATCH_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (buf == NULL) {
        ESP_LOGW(TAG, "Stereo scratch aligned alloc failed (%u) - attempting PSRAM", (unsigned int)CONFIG_TTS_STEREO_SCRATCH_BYTES);
        buf = heap_caps_aligned_alloc(4, CONFIG_TTS_STEREO_SCRATCH_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
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

    ESP_LOGI(TAG, "[PCM DUP] Scratch buffer ready: %zu bytes (%zu samples per block)",
             s_stereo_scratch_size, s_stereo_scratch_capacity_samples);
    return true;
}

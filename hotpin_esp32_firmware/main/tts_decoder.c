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
#include "websocket_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>

static const char *TAG = TAG_TTS;

// WAV header structure (44 bytes)
typedef struct __attribute__((packed)) {
    // RIFF header
    char riff[4];           // "RIFF"
    uint32_t file_size;     // File size - 8
    char wave[4];           // "WAVE"
    
    // fmt subchunk
    char fmt[4];            // "fmt "
    uint32_t fmt_size;      // 16 for PCM
    uint16_t audio_format;  // 1 for PCM
    uint16_t num_channels;  // 1 = mono, 2 = stereo
    uint32_t sample_rate;   // Sampling rate (e.g., 16000, 22050)
    uint32_t byte_rate;     // sample_rate * num_channels * bits_per_sample / 8
    uint16_t block_align;   // num_channels * bits_per_sample / 8
    uint16_t bits_per_sample; // 8, 16, 32
    
    // data subchunk
    char data[4];           // "data"
    uint32_t data_size;     // Size of PCM data
} wav_header_t;

// Audio buffer queue
#define AUDIO_QUEUE_SIZE        10
#define AUDIO_CHUNK_SIZE        4096

typedef struct {
    uint8_t *data;
    size_t length;
} audio_chunk_t;

// State management
static bool is_initialized = false;
static bool is_playing = false;
static bool is_running = false;
static bool header_parsed = false;

// WAV header info
static wav_header_t wav_info;
static size_t bytes_received = 0;
static size_t pcm_bytes_played = 0;

// Playback queue and task
static QueueHandle_t g_audio_queue = NULL;
static TaskHandle_t g_playback_task_handle = NULL;

// Buffer for header accumulation
static uint8_t header_buffer[44];
static size_t header_bytes_received = 0;

// ===========================
// Private Function Declarations
// ===========================
static void tts_playback_task(void *pvParameters);
static esp_err_t parse_wav_header(const uint8_t *header);
static void print_wav_info(const wav_header_t *info);
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
    
    // Wait for task to finish
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // Delete playback task
    if (g_playback_task_handle != NULL) {
        vTaskDelete(g_playback_task_handle);
        g_playback_task_handle = NULL;
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
    
    // Allocate memory for chunk
    uint8_t *chunk_data = heap_caps_malloc(len, MALLOC_CAP_INTERNAL);
    if (chunk_data == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for audio chunk");
        return;
    }
    
    memcpy(chunk_data, data, len);
    
    // Create chunk descriptor
    audio_chunk_t chunk = {
        .data = chunk_data,
        .length = len
    };
    
    // Send to playback queue
    if (xQueueSend(g_audio_queue, &chunk, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Audio queue full - dropping chunk");
        heap_caps_free(chunk_data);
    }
    
    bytes_received += len;
}

static void tts_playback_task(void *pvParameters) {
    ESP_LOGI(TAG, "TTS playback task started on Core %d", xPortGetCoreID());
    
    audio_chunk_t chunk;
    
    while (is_running) {
        // Wait for audio chunk
        if (xQueueReceive(g_audio_queue, &chunk, pdMS_TO_TICKS(100)) == pdTRUE) {
            
            // Parse header if not done yet
            if (!header_parsed) {
                // Accumulate header bytes
                size_t to_copy = 44 - header_bytes_received;
                if (to_copy > chunk.length) {
                    to_copy = chunk.length;
                }
                
                memcpy(header_buffer + header_bytes_received, chunk.data, to_copy);
                header_bytes_received += to_copy;
                
                // Check if header complete
                if (header_bytes_received >= 44) {
                    esp_err_t ret = parse_wav_header(header_buffer);
                    if (ret == ESP_OK) {
                        header_parsed = true;
                        print_wav_info(&wav_info);
                        
                        // Play remaining PCM data from this chunk
                        if (chunk.length > to_copy) {
                            size_t bytes_written = 0;
                            audio_driver_write(chunk.data + to_copy, chunk.length - to_copy, 
                                               &bytes_written, portMAX_DELAY);
                            pcm_bytes_played += bytes_written;
                        }
                    } else {
                        ESP_LOGE(TAG, "Failed to parse WAV header");
                        is_running = false;
                    }
                }
                
            } else {
                // Header already parsed - play PCM data directly
                size_t bytes_written = 0;
                esp_err_t ret = audio_driver_write(chunk.data, chunk.length, 
                                                     &bytes_written, portMAX_DELAY);
                
                if (ret == ESP_OK) {
                    pcm_bytes_played += bytes_written;
                    ESP_LOGD(TAG, "Played %zu bytes (total: %zu)", bytes_written, pcm_bytes_played);
                } else {
                    ESP_LOGE(TAG, "Audio playback error: %s", esp_err_to_name(ret));
                }
            }
            
            // Free chunk memory
            heap_caps_free(chunk.data);
        }
    }
    
    ESP_LOGI(TAG, "TTS playback task stopped");
    vTaskDelete(NULL);
}

static esp_err_t parse_wav_header(const uint8_t *header) {
    ESP_LOGI(TAG, "Parsing WAV header...");
    
    memcpy(&wav_info, header, sizeof(wav_header_t));
    
    // Validate RIFF header
    if (strncmp(wav_info.riff, "RIFF", 4) != 0) {
        ESP_LOGE(TAG, "Invalid RIFF header");
        return ESP_FAIL;
    }
    
    if (strncmp(wav_info.wave, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "Invalid WAVE header");
        return ESP_FAIL;
    }
    
    // Validate fmt chunk
    if (strncmp(wav_info.fmt, "fmt ", 4) != 0) {
        ESP_LOGE(TAG, "Invalid fmt chunk");
        return ESP_FAIL;
    }
    
    if (wav_info.audio_format != 1) {
        ESP_LOGE(TAG, "Unsupported audio format: %d (only PCM=1 supported)", wav_info.audio_format);
        return ESP_FAIL;
    }
    
    // Validate data chunk
    if (strncmp(wav_info.data, "data", 4) != 0) {
        ESP_LOGE(TAG, "Invalid data chunk");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "✅ WAV header parsed successfully");
    return ESP_OK;
}

static void print_wav_info(const wav_header_t *info) {
    ESP_LOGI(TAG, "=== WAV File Info ===");
    ESP_LOGI(TAG, "Sample Rate: %lu Hz", info->sample_rate);
    ESP_LOGI(TAG, "Channels: %d", info->num_channels);
    ESP_LOGI(TAG, "Bits per Sample: %d", info->bits_per_sample);
    ESP_LOGI(TAG, "Audio Format: %d (PCM)", info->audio_format);
    ESP_LOGI(TAG, "Data Size: %lu bytes", info->data_size);
    ESP_LOGI(TAG, "====================");
}

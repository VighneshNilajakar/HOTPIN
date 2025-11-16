/**
 * @file feedback_player.c
 * @brief Programmatic audio tone playback for system feedback cues.
 */

#include "feedback_player.h"
#include "audio_driver.h"
#include "config.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>
#include <stdbool.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FEEDBACK_SAMPLE_RATE          CONFIG_AUDIO_SAMPLE_RATE
#define FEEDBACK_MAX_SEGMENT_MS        600U
#define FEEDBACK_CHANNELS              2U
#define FEEDBACK_MAX_SEGMENT_FRAMES   ((FEEDBACK_SAMPLE_RATE * FEEDBACK_MAX_SEGMENT_MS) / 1000U)
#define FEEDBACK_MAX_SEGMENT_SAMPLES  (FEEDBACK_MAX_SEGMENT_FRAMES * FEEDBACK_CHANNELS)
#define FEEDBACK_DEFAULT_VOLUME        0.60f
#define FEEDBACK_LOW_VOLUME            0.45f

// Nokia Tune notes (based on Francisco Tárrega's Gran Vals)
#define NOTE_E5   659.26f  // Mi
#define NOTE_D5   587.33f  // Re
#define NOTE_FS4  369.99f  // Fa#
#define NOTE_GS4  415.30f  // Sol#
#define NOTE_CS5  554.37f  // Do#
#define NOTE_B4   493.88f  // Si
#define NOTE_D4   293.66f  // Re
#define NOTE_E4   329.63f  // Mi
#define NOTE_A4   440.00f  // La (for variations)
#define NOTE_C4   261.63f  // Do (for error tones)

typedef struct {
    bool is_noise;
    float primary_freq_hz;
    float secondary_freq_hz;
    uint32_t duration_ms;
    float amplitude;
} tone_segment_t;

static const char *TAG = "FEEDBACK_PLAYER";
static SemaphoreHandle_t s_play_mutex = NULL;
static bool s_initialized = false;
static int16_t s_work_buffer[FEEDBACK_MAX_SEGMENT_SAMPLES] DRAM_ATTR __attribute__((aligned(16)));

// Phase continuity for smooth transitions
static float s_phase_a = 0.0f;
static float s_phase_b = 0.0f;

// Envelope constants for fade-in/fade-out (reduces clicks)
#define ENVELOPE_FADE_MS 5
#define ENVELOPE_FADE_SAMPLES ((FEEDBACK_SAMPLE_RATE * ENVELOPE_FADE_MS) / 1000)

static esp_err_t ensure_initialized(void);
static esp_err_t play_segments(const tone_segment_t *segments, size_t count);

// Provided by main.c to coordinate audio/camera reconfiguration
extern SemaphoreHandle_t g_i2s_config_mutex;

// Classic Nokia startup (first 4 notes of Gran Vals)
static const tone_segment_t BOOT_SEQUENCE[] = {
    {.is_noise = false, .primary_freq_hz = NOTE_E5, .secondary_freq_hz = 0.0f, .duration_ms = 125, .amplitude = FEEDBACK_DEFAULT_VOLUME},
    {.is_noise = false, .primary_freq_hz = NOTE_D5, .secondary_freq_hz = 0.0f, .duration_ms = 125, .amplitude = FEEDBACK_DEFAULT_VOLUME},
    {.is_noise = false, .primary_freq_hz = NOTE_FS4, .secondary_freq_hz = 0.0f, .duration_ms = 250, .amplitude = FEEDBACK_DEFAULT_VOLUME},
    {.is_noise = false, .primary_freq_hz = NOTE_GS4, .secondary_freq_hz = 0.0f, .duration_ms = 250, .amplitude = FEEDBACK_DEFAULT_VOLUME},
};

// Shutdown: reverse of Nokia tune (descending)
static const tone_segment_t SHUTDOWN_SEQUENCE[] = {
    {.is_noise = false, .primary_freq_hz = NOTE_GS4, .secondary_freq_hz = 0.0f, .duration_ms = 200, .amplitude = FEEDBACK_DEFAULT_VOLUME},
    {.is_noise = false, .primary_freq_hz = NOTE_FS4, .secondary_freq_hz = 0.0f, .duration_ms = 200, .amplitude = FEEDBACK_DEFAULT_VOLUME},
    {.is_noise = false, .primary_freq_hz = NOTE_D5, .secondary_freq_hz = 0.0f, .duration_ms = 150, .amplitude = FEEDBACK_DEFAULT_VOLUME},
    {.is_noise = false, .primary_freq_hz = NOTE_E5, .secondary_freq_hz = 0.0f, .duration_ms = 300, .amplitude = FEEDBACK_LOW_VOLUME},
};

// Error: two descending notes (D5-C4) - Nokia-style alert
static const tone_segment_t ERROR_SEQUENCE[] = {
    {.is_noise = false, .primary_freq_hz = NOTE_D5, .secondary_freq_hz = 0.0f, .duration_ms = 200, .amplitude = FEEDBACK_LOW_VOLUME},
    {.is_noise = false, .primary_freq_hz = NOTE_C4, .secondary_freq_hz = 0.0f, .duration_ms = 400, .amplitude = FEEDBACK_LOW_VOLUME},
};

// Recording start: quick ascending (E4-A4) - Nokia notification style
static const tone_segment_t REC_START_SEQUENCE[] = {
    {.is_noise = false, .primary_freq_hz = NOTE_E4, .secondary_freq_hz = 0.0f, .duration_ms = 80, .amplitude = FEEDBACK_DEFAULT_VOLUME},
    {.is_noise = false, .primary_freq_hz = NOTE_A4, .secondary_freq_hz = 0.0f, .duration_ms = 120, .amplitude = FEEDBACK_DEFAULT_VOLUME},
};

// Recording stop: quick descending (A4-E4) - Nokia confirmation
static const tone_segment_t REC_STOP_SEQUENCE[] = {
    {.is_noise = false, .primary_freq_hz = NOTE_A4, .secondary_freq_hz = 0.0f, .duration_ms = 80, .amplitude = FEEDBACK_DEFAULT_VOLUME},
    {.is_noise = false, .primary_freq_hz = NOTE_E4, .secondary_freq_hz = 0.0f, .duration_ms = 120, .amplitude = FEEDBACK_DEFAULT_VOLUME},
};

// Camera capture: short shutter sound (white noise is appropriate for camera click)
static const tone_segment_t CAPTURE_SEQUENCE[] = {
    {.is_noise = true, .primary_freq_hz = 0.0f, .secondary_freq_hz = 0.0f, .duration_ms = 90, .amplitude = FEEDBACK_DEFAULT_VOLUME},
};

// Processing: Nokia tune fragment (E5-D5) - "working..."
static const tone_segment_t PROCESSING_SEQUENCE[] = {
    {.is_noise = false, .primary_freq_hz = NOTE_E5, .secondary_freq_hz = 0.0f, .duration_ms = 100, .amplitude = FEEDBACK_DEFAULT_VOLUME},
    {.is_noise = false, .primary_freq_hz = NOTE_D5, .secondary_freq_hz = 0.0f, .duration_ms = 100, .amplitude = FEEDBACK_DEFAULT_VOLUME},
};

// TTS complete: Full Nokia tune (second half: C#5-B4-D4-E4) - "ready!"
static const tone_segment_t TTS_COMPLETE_SEQUENCE[] = {
    {.is_noise = false, .primary_freq_hz = NOTE_CS5, .secondary_freq_hz = 0.0f, .duration_ms = 125, .amplitude = FEEDBACK_DEFAULT_VOLUME},
    {.is_noise = false, .primary_freq_hz = NOTE_B4, .secondary_freq_hz = 0.0f, .duration_ms = 125, .amplitude = FEEDBACK_DEFAULT_VOLUME},
    {.is_noise = false, .primary_freq_hz = NOTE_D4, .secondary_freq_hz = 0.0f, .duration_ms = 250, .amplitude = FEEDBACK_DEFAULT_VOLUME},
    {.is_noise = false, .primary_freq_hz = NOTE_E4, .secondary_freq_hz = 0.0f, .duration_ms = 250, .amplitude = FEEDBACK_DEFAULT_VOLUME},
};

esp_err_t feedback_player_init(void) {
    if (s_initialized) {
        return ESP_OK;
    }

    s_play_mutex = xSemaphoreCreateMutex();
    if (s_play_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create playback mutex");
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    return ESP_OK;
}

esp_err_t feedback_player_play(feedback_sound_t sound) {
    esp_err_t ret = ensure_initialized();
    if (ret != ESP_OK) {
        return ret;
    }

    if (xSemaphoreTake(s_play_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        ESP_LOGW(TAG, "Timed out waiting for playback mutex");
        return ESP_ERR_TIMEOUT;
    }

    const tone_segment_t *sequence = NULL;
    size_t count = 0U;
    bool config_mutex_taken = false;

    switch (sound) {
        case FEEDBACK_SOUND_BOOT:
            sequence = BOOT_SEQUENCE;
            count = sizeof(BOOT_SEQUENCE) / sizeof(BOOT_SEQUENCE[0]);
            break;
        case FEEDBACK_SOUND_SHUTDOWN:
            sequence = SHUTDOWN_SEQUENCE;
            count = sizeof(SHUTDOWN_SEQUENCE) / sizeof(SHUTDOWN_SEQUENCE[0]);
            break;
        case FEEDBACK_SOUND_ERROR:
            sequence = ERROR_SEQUENCE;
            count = sizeof(ERROR_SEQUENCE) / sizeof(ERROR_SEQUENCE[0]);
            break;
        case FEEDBACK_SOUND_REC_START:
            sequence = REC_START_SEQUENCE;
            count = sizeof(REC_START_SEQUENCE) / sizeof(REC_START_SEQUENCE[0]);
            break;
        case FEEDBACK_SOUND_REC_STOP:
            sequence = REC_STOP_SEQUENCE;
            count = sizeof(REC_STOP_SEQUENCE) / sizeof(REC_STOP_SEQUENCE[0]);
            break;
        case FEEDBACK_SOUND_CAPTURE:
            sequence = CAPTURE_SEQUENCE;
            count = sizeof(CAPTURE_SEQUENCE) / sizeof(CAPTURE_SEQUENCE[0]);
            break;
        case FEEDBACK_SOUND_PROCESSING:
            sequence = PROCESSING_SEQUENCE;
            count = sizeof(PROCESSING_SEQUENCE) / sizeof(PROCESSING_SEQUENCE[0]);
            break;
        case FEEDBACK_SOUND_TTS_COMPLETE:
            sequence = TTS_COMPLETE_SEQUENCE;
            count = sizeof(TTS_COMPLETE_SEQUENCE) / sizeof(TTS_COMPLETE_SEQUENCE[0]);
            break;
        default:
            ESP_LOGE(TAG, "Invalid sound id: %d", sound);
            xSemaphoreGive(s_play_mutex);
            return ESP_ERR_INVALID_ARG;
    }

    bool driver_was_initialized = audio_driver_is_initialized();
    bool driver_initialized_here = false;
    uint32_t total_duration_ms = 0U;

    for (size_t i = 0; i < count; ++i) {
        total_duration_ms += sequence[i].duration_ms;
    }

    if (g_i2s_config_mutex != NULL) {
        if (xSemaphoreTake(g_i2s_config_mutex, pdMS_TO_TICKS(750)) != pdTRUE) {
            ESP_LOGW(TAG, "Failed to acquire configuration mutex for playback");
            xSemaphoreGive(s_play_mutex);
            return ESP_ERR_TIMEOUT;
        }
        config_mutex_taken = true;
    }

    if (!driver_was_initialized) {
        // ✅ FIX #3: Check BOTH total DMA memory AND largest contiguous block
        // I2S full-duplex driver needs: TX (~8KB) + RX (~8KB) + overhead = ~18KB total
        // High fragmentation can cause init failure even with sufficient total memory
        // Check largest block to ensure contiguous allocation succeeds
        size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
        size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
        const size_t MIN_DMA_TOTAL = 20480;     // 20KB total minimum
        const size_t MIN_DMA_CONTIGUOUS = 16384; // 16KB contiguous block minimum
        
        if (dma_free < MIN_DMA_TOTAL) {
            ESP_LOGW(TAG, "Insufficient DMA memory for audio driver (%zu bytes free, need %zu) - skipping feedback",
                     dma_free, MIN_DMA_TOTAL);
            ret = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        
        if (largest_block < MIN_DMA_CONTIGUOUS) {
            ESP_LOGW(TAG, "DMA memory too fragmented for audio driver (largest block: %zu bytes, need %zu) - skipping feedback",
                     largest_block, MIN_DMA_CONTIGUOUS);
            ret = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        
        ret = audio_driver_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to init audio driver for feedback: %s", esp_err_to_name(ret));
            goto cleanup;
        }
        driver_initialized_here = true;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ret = play_segments(sequence, count);

    if (ret == ESP_OK && total_duration_ms > 0U) {
        uint32_t settle_time_ms = total_duration_ms + 120U;
        vTaskDelay(pdMS_TO_TICKS(settle_time_ms));
    }

    if (driver_initialized_here) {
        vTaskDelay(pdMS_TO_TICKS(40));
        audio_driver_deinit();
    }

cleanup:
    if (config_mutex_taken) {
        xSemaphoreGive(g_i2s_config_mutex);
    }

    xSemaphoreGive(s_play_mutex);
    return ret;
}

static esp_err_t ensure_initialized(void) {
    if (!s_initialized) {
        return feedback_player_init();
    }
    return ESP_OK;
}

static inline int16_t float_to_sample(float value) {
    if (value > 1.0f) {
        value = 1.0f;
    } else if (value < -1.0f) {
        value = -1.0f;
    }
    return (int16_t)(value * 32767.0f);
}

static void generate_noise_samples(size_t frame_count, float amplitude) {
    const float scale = amplitude;
    size_t idx = 0;
    for (size_t i = 0; i < frame_count; ++i) {
        int32_t random_value = (int32_t)(esp_random() & 0xFFFF);
        float normalized = ((float)random_value / 32768.0f) - 1.0f;
        int16_t sample = float_to_sample(normalized * scale);
        s_work_buffer[idx++] = sample;
        s_work_buffer[idx++] = sample;
    }
}

static void generate_tone_samples(size_t frame_count, float freq_a, float freq_b, float amplitude) {
    const float omega_a = (freq_a > 0.0f) ? (2.0f * (float)M_PI * freq_a / (float)FEEDBACK_SAMPLE_RATE) : 0.0f;
    const float omega_b = (freq_b > 0.0f) ? (2.0f * (float)M_PI * freq_b / (float)FEEDBACK_SAMPLE_RATE) : 0.0f;
    const bool has_dual = (omega_a > 0.0f && omega_b > 0.0f);
    const float mix_scale = has_dual ? 0.5f : 1.0f;  // Prevent clipping with dual tones
    size_t idx = 0;

    // Calculate fade envelope samples
    size_t fade_samples = (frame_count < ENVELOPE_FADE_SAMPLES * 2) ? (frame_count / 4) : ENVELOPE_FADE_SAMPLES;

    for (size_t i = 0; i < frame_count; ++i) {
        float sample = 0.0f;
        
        // Generate tones with proper mixing
        if (omega_a > 0.0f) {
            sample += sinf(s_phase_a);
            s_phase_a += omega_a;
            // Keep phase in range to prevent float precision issues
            if (s_phase_a > (2.0f * (float)M_PI)) {
                s_phase_a -= (2.0f * (float)M_PI);
            }
        }
        if (omega_b > 0.0f) {
            sample += sinf(s_phase_b);
            s_phase_b += omega_b;
            if (s_phase_b > (2.0f * (float)M_PI)) {
                s_phase_b -= (2.0f * (float)M_PI);
            }
        }

        // Apply envelope for smooth fade-in/fade-out
        float envelope = 1.0f;
        if (i < fade_samples) {
            envelope = (float)i / (float)fade_samples;  // Fade-in
        } else if (i >= frame_count - fade_samples) {
            envelope = (float)(frame_count - i) / (float)fade_samples;  // Fade-out
        }

        // Mix and scale properly
        sample = sample * mix_scale * amplitude * envelope;
        
        int16_t rendered = float_to_sample(sample);
        s_work_buffer[idx++] = rendered;
        s_work_buffer[idx++] = rendered;
    }
}

static esp_err_t play_segments(const tone_segment_t *segments, size_t count) {
    // Reset phase for new sound sequence (maintains continuity within sequence)
    s_phase_a = 0.0f;
    s_phase_b = 0.0f;

    for (size_t i = 0; i < count; ++i) {
        const tone_segment_t *segment = &segments[i];
        if (segment->duration_ms == 0U) {
            continue;
        }

        size_t frame_count = (FEEDBACK_SAMPLE_RATE * segment->duration_ms) / 1000U;
        if (frame_count == 0U) {
            continue;
        }
        if (frame_count > FEEDBACK_MAX_SEGMENT_FRAMES) {
            ESP_LOGW(TAG, "Segment duration too long (%u ms) - truncating", (unsigned int)segment->duration_ms);
            frame_count = FEEDBACK_MAX_SEGMENT_FRAMES;
        }

        if (segment->is_noise) {
            generate_noise_samples(frame_count, segment->amplitude);
        } else if (segment->primary_freq_hz <= 0.0f && segment->secondary_freq_hz <= 0.0f) {
            memset(s_work_buffer, 0, frame_count * FEEDBACK_CHANNELS * sizeof(int16_t));
        } else {
            generate_tone_samples(frame_count, segment->primary_freq_hz, segment->secondary_freq_hz, segment->amplitude);
        }

        const size_t byte_count = frame_count * FEEDBACK_CHANNELS * sizeof(int16_t);
        size_t total_written = 0U;

        while (total_written < byte_count) {
            size_t written = 0U;
            esp_err_t write_ret = audio_driver_write((const uint8_t *)s_work_buffer + total_written,
                                                     byte_count - total_written,
                                                     &written,
                                                     200);
            if (write_ret != ESP_OK) {
                ESP_LOGE(TAG, "Audio write failed: %s", esp_err_to_name(write_ret));
                return write_ret;
            }
            if (written == 0U) {
                ESP_LOGE(TAG, "Audio write returned zero bytes");
                return ESP_FAIL;
            }
            total_written += written;
        }
    }

    return ESP_OK;
}

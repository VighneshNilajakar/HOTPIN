/**
 * @file feedback_player.c
 * @brief Programmatic audio tone playback for system feedback cues.
 */

#include "feedback_player.h"
#include "audio_driver.h"
#include "config.h"
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
#define FEEDBACK_MAX_SEGMENT_MS       600U
#define FEEDBACK_MAX_SEGMENT_SAMPLES  ((FEEDBACK_SAMPLE_RATE * FEEDBACK_MAX_SEGMENT_MS) / 1000U)
#define FEEDBACK_DEFAULT_VOLUME       0.45f
#define FEEDBACK_LOW_VOLUME           0.35f

#define NOTE_C4   261.63f
#define NOTE_E4   329.63f
#define NOTE_G4   392.00f
#define NOTE_G5   783.99f
#define NOTE_E5   659.26f
#define NOTE_C3   130.81f
#define NOTE_DS3  155.56f

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
static int16_t s_work_buffer[FEEDBACK_MAX_SEGMENT_SAMPLES];

static esp_err_t ensure_initialized(void);
static esp_err_t play_segments(const tone_segment_t *segments, size_t count);

// Provided by main.c to coordinate audio/camera reconfiguration
extern SemaphoreHandle_t g_i2s_config_mutex;

static const tone_segment_t BOOT_SEQUENCE[] = {
    {.is_noise = false, .primary_freq_hz = NOTE_C4, .secondary_freq_hz = 0.0f, .duration_ms = 180, .amplitude = FEEDBACK_DEFAULT_VOLUME},
    {.is_noise = false, .primary_freq_hz = 0.0f,  .secondary_freq_hz = 0.0f, .duration_ms = 40,  .amplitude = 0.0f},
    {.is_noise = false, .primary_freq_hz = NOTE_E4, .secondary_freq_hz = 0.0f, .duration_ms = 180, .amplitude = FEEDBACK_DEFAULT_VOLUME},
    {.is_noise = false, .primary_freq_hz = 0.0f,  .secondary_freq_hz = 0.0f, .duration_ms = 40,  .amplitude = 0.0f},
    {.is_noise = false, .primary_freq_hz = NOTE_G4, .secondary_freq_hz = 0.0f, .duration_ms = 220, .amplitude = FEEDBACK_DEFAULT_VOLUME},
};

static const tone_segment_t SHUTDOWN_SEQUENCE[] = {
    {.is_noise = false, .primary_freq_hz = NOTE_G4, .secondary_freq_hz = 0.0f, .duration_ms = 200, .amplitude = FEEDBACK_DEFAULT_VOLUME},
    {.is_noise = false, .primary_freq_hz = NOTE_E4, .secondary_freq_hz = 0.0f, .duration_ms = 200, .amplitude = FEEDBACK_DEFAULT_VOLUME},
    {.is_noise = false, .primary_freq_hz = NOTE_C4, .secondary_freq_hz = 0.0f, .duration_ms = 240, .amplitude = FEEDBACK_DEFAULT_VOLUME},
};

static const tone_segment_t ERROR_SEQUENCE[] = {
    {.is_noise = false, .primary_freq_hz = NOTE_C3, .secondary_freq_hz = NOTE_DS3, .duration_ms = 520, .amplitude = FEEDBACK_LOW_VOLUME},
    {.is_noise = false, .primary_freq_hz = 0.0f,   .secondary_freq_hz = 0.0f,   .duration_ms = 120, .amplitude = 0.0f},
};

static const tone_segment_t REC_START_SEQUENCE[] = {
    {.is_noise = false, .primary_freq_hz = NOTE_G5, .secondary_freq_hz = NOTE_E5, .duration_ms = 120, .amplitude = FEEDBACK_DEFAULT_VOLUME},
};

static const tone_segment_t REC_STOP_SEQUENCE[] = {
    {.is_noise = false, .primary_freq_hz = NOTE_C4, .secondary_freq_hz = 0.0f, .duration_ms = 110, .amplitude = FEEDBACK_DEFAULT_VOLUME},
};

static const tone_segment_t CAPTURE_SEQUENCE[] = {
    {.is_noise = true, .primary_freq_hz = 0.0f, .secondary_freq_hz = 0.0f, .duration_ms = 160, .amplitude = FEEDBACK_DEFAULT_VOLUME},
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

static void generate_noise_samples(size_t sample_count, float amplitude) {
    const float scale = amplitude;
    for (size_t i = 0; i < sample_count; ++i) {
        int32_t random_value = (int32_t)(esp_random() & 0xFFFF);
        float normalized = ((float)random_value / 32768.0f) - 1.0f;
        s_work_buffer[i] = float_to_sample(normalized * scale);
    }
}

static void generate_tone_samples(size_t sample_count, float freq_a, float freq_b, float amplitude) {
    float phase_a = 0.0f;
    float phase_b = 0.0f;
    const float omega_a = (freq_a > 0.0f) ? (2.0f * (float)M_PI * freq_a / (float)FEEDBACK_SAMPLE_RATE) : 0.0f;
    const float omega_b = (freq_b > 0.0f) ? (2.0f * (float)M_PI * freq_b / (float)FEEDBACK_SAMPLE_RATE) : 0.0f;

    for (size_t i = 0; i < sample_count; ++i) {
        float sample = 0.0f;
        if (omega_a > 0.0f) {
            sample += sinf(phase_a);
            phase_a += omega_a;
        }
        if (omega_b > 0.0f) {
            sample += 0.6f * sinf(phase_b);
            phase_b += omega_b;
        }
        s_work_buffer[i] = float_to_sample(sample * amplitude);
    }
}

static esp_err_t play_segments(const tone_segment_t *segments, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const tone_segment_t *segment = &segments[i];
        if (segment->duration_ms == 0U) {
            continue;
        }

        size_t sample_count = (FEEDBACK_SAMPLE_RATE * segment->duration_ms) / 1000U;
        if (sample_count == 0U) {
            continue;
        }
        if (sample_count > FEEDBACK_MAX_SEGMENT_SAMPLES) {
            ESP_LOGW(TAG, "Segment duration too long (%u ms) - truncating", (unsigned int)segment->duration_ms);
            sample_count = FEEDBACK_MAX_SEGMENT_SAMPLES;
        }

        if (segment->is_noise) {
            generate_noise_samples(sample_count, segment->amplitude);
        } else if (segment->primary_freq_hz <= 0.0f && segment->secondary_freq_hz <= 0.0f) {
            memset(s_work_buffer, 0, sample_count * sizeof(int16_t));
        } else {
            generate_tone_samples(sample_count, segment->primary_freq_hz, segment->secondary_freq_hz, segment->amplitude);
        }

        const size_t byte_count = sample_count * sizeof(int16_t);
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

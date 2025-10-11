/**
 * @file audio_feedback.c
 * @brief Simple audible feedback patterns for HotPin device states.
 */

#include "audio_feedback.h"
#include "audio_driver.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>

#define FEEDBACK_SAMPLE_RATE     CONFIG_AUDIO_SAMPLE_RATE
#define FEEDBACK_TONE_FREQUENCY  1400.0f   // Hz
#define FEEDBACK_TONE_DURATION_MS 120      // milliseconds per beep
#define FEEDBACK_SILENCE_MS       90       // gap between beeps
#define FEEDBACK_VOLUME           0.35f    // scaled [-1.0,1.0]

static const char *TAG = "AUDIO_FEEDBACK";

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static bool s_waveform_ready = false;
static int16_t s_beep_waveform[(FEEDBACK_SAMPLE_RATE * FEEDBACK_TONE_DURATION_MS) / 1000] __attribute__((aligned(4)));

static void audio_feedback_prepare_waveform(void) {
    if (s_waveform_ready) {
        return;
    }

    const size_t sample_count = sizeof(s_beep_waveform) / sizeof(s_beep_waveform[0]);
    const float angular_step = (2.0f * (float)M_PI * FEEDBACK_TONE_FREQUENCY) / (float)FEEDBACK_SAMPLE_RATE;

    for (size_t i = 0; i < sample_count; ++i) {
        float value = sinf(angular_step * (float)i);
        int32_t sample = (int32_t)(value * FEEDBACK_VOLUME * 32767.0f);
        if (sample > 32767) {
            sample = 32767;
        } else if (sample < -32768) {
            sample = -32768;
        }
        s_beep_waveform[i] = (int16_t)sample;
    }

    s_waveform_ready = true;
}

static esp_err_t audio_feedback_emit_beep(bool allow_temp_driver) {
    esp_err_t ret;
    bool driver_was_initialized = audio_driver_is_initialized();
    bool driver_initialized_here = false;

    if (!driver_was_initialized) {
        if (!allow_temp_driver) {
            ESP_LOGW(TAG, "Audio driver not available for feedback");
            return ESP_ERR_INVALID_STATE;
        }
        ret = audio_driver_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to init audio driver for feedback: %s", esp_err_to_name(ret));
            return ret;
        }
        driver_initialized_here = true;
        // Small delay to let the I2S hardware settle before writing data.
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    audio_feedback_prepare_waveform();

    size_t bytes_written = 0;
    ret = audio_driver_write((const uint8_t *)s_beep_waveform,
                             sizeof(s_beep_waveform),
                             &bytes_written,
                             200);

    if (ret != ESP_OK || bytes_written != sizeof(s_beep_waveform)) {
        ESP_LOGE(TAG, "Beep write failed (%s), wrote %zu/%zu bytes",
                 esp_err_to_name(ret), bytes_written, sizeof(s_beep_waveform));
        if (ret == ESP_OK && bytes_written != sizeof(s_beep_waveform)) {
            ret = ESP_FAIL;
        }
    }

    if (driver_initialized_here) {
        // Allow FIFO to drain before shutting back down.
        vTaskDelay(pdMS_TO_TICKS(20));
        audio_driver_deinit();
    }

    return ret;
}

esp_err_t audio_feedback_play_pattern(audio_feedback_pattern_t pattern,
                                       bool allow_temp_driver) {
    const uint8_t beep_count = (pattern == AUDIO_FEEDBACK_PATTERN_DOUBLE) ? 2 :
                               (pattern == AUDIO_FEEDBACK_PATTERN_TRIPLE) ? 3 : 1;

    esp_err_t last_ret = ESP_OK;

    for (uint8_t i = 0; i < beep_count; ++i) {
        last_ret = audio_feedback_emit_beep(allow_temp_driver);
        if (last_ret != ESP_OK) {
            break;
        }
        if (i + 1u < beep_count) {
            vTaskDelay(pdMS_TO_TICKS(FEEDBACK_SILENCE_MS));
        }
    }

    return last_ret;
}

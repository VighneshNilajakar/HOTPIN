/**
 * @file audio_feedback.c
 * @brief Simple audible feedback patterns for HotPin device states.
 */

#include "audio_feedback.h"
#include "audio_driver.h"
#include "config.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>

#define FEEDBACK_SAMPLE_RATE       CONFIG_AUDIO_SAMPLE_RATE
#define FEEDBACK_TONE_FREQUENCY    1400.0f   // Hz
#define FEEDBACK_TONE_DURATION_MS  120       // milliseconds per beep
#define FEEDBACK_SILENCE_MS        90        // gap between beeps
#define FEEDBACK_VOLUME            0.45f     // scaled [-1.0,1.0]
#define FEEDBACK_CHANNELS          2U

static const char *TAG = "AUDIO_FEEDBACK";

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static bool s_waveform_ready = false;
static int16_t s_beep_waveform[((FEEDBACK_SAMPLE_RATE * FEEDBACK_TONE_DURATION_MS) / 1000) * FEEDBACK_CHANNELS] DRAM_ATTR __attribute__((aligned(16)));
static uint32_t s_beep_debug_logs = 0;

static void audio_feedback_prepare_waveform(void) {
    if (s_waveform_ready) {
        return;
    }

    const size_t frame_count = (sizeof(s_beep_waveform) / sizeof(int16_t)) / FEEDBACK_CHANNELS;
    const float angular_step = (2.0f * (float)M_PI * FEEDBACK_TONE_FREQUENCY) / (float)FEEDBACK_SAMPLE_RATE;

    size_t idx = 0;
    for (size_t i = 0; i < frame_count; ++i) {
        float value = sinf(angular_step * (float)i);
        int32_t sample = (int32_t)(value * FEEDBACK_VOLUME * 32767.0f);
        if (sample > 32767) {
            sample = 32767;
        } else if (sample < -32768) {
            sample = -32768;
        }
        int16_t rendered = (int16_t)sample;
        s_beep_waveform[idx++] = rendered;
        s_beep_waveform[idx++] = rendered;
    }

    s_waveform_ready = true;
}

static esp_err_t audio_feedback_emit_beep(bool allow_temp_driver) {
    esp_err_t ret;
    bool driver_was_initialized = audio_driver_is_initialized();
    bool driver_initialized_here = false;
    const size_t payload_bytes = sizeof(s_beep_waveform);
    const bool should_log_debug = (s_beep_debug_logs < 6);

    if (!driver_was_initialized) {
        if (!allow_temp_driver) {
            ESP_LOGW(TAG, "Audio driver not available for feedback");
            return ESP_ERR_INVALID_STATE;
        }
        
        // ✅ FIX #12: Check DMA-capable memory before attempting audio driver init
        // I2S driver needs ~8-10KB of DMA memory for buffers
        // If insufficient memory, skip feedback gracefully instead of failing
        size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
        const size_t MIN_DMA_REQUIRED = 20480; // 20KB minimum (with safety margin)
        
        if (dma_free < MIN_DMA_REQUIRED) {
            ESP_LOGW(TAG, "Insufficient DMA memory for audio driver (%zu bytes free, need %zu) - skipping feedback",
                     dma_free, MIN_DMA_REQUIRED);
            return ESP_ERR_NO_MEM;
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
    if (should_log_debug) {
        ESP_LOGD(TAG, "[BEEP] start allow_temp=%d driver_pre_init=%d bytes=%zu",
                 allow_temp_driver, driver_was_initialized, payload_bytes);
    }

    ret = audio_driver_write((const uint8_t *)s_beep_waveform,
                             payload_bytes,
                             &bytes_written,
                             200);

    if (ret != ESP_OK || bytes_written != payload_bytes) {
        ESP_LOGE(TAG, "Beep write failed (%s), wrote %zu/%zu bytes",
                 esp_err_to_name(ret), bytes_written, payload_bytes);
        if (ret == ESP_OK && bytes_written != payload_bytes) {
            ret = ESP_FAIL;
        }
    } else if (should_log_debug) {
        ESP_LOGD(TAG, "[BEEP] complete wrote=%zu bytes temp_driver=%d",
                 bytes_written, driver_initialized_here);
    }

    if (should_log_debug) {
        s_beep_debug_logs++;
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

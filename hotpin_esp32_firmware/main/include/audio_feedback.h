/**
 * @file audio_feedback.h
 * @brief Simple audio feedback patterns rendered over the MAX98357A speaker.
 */

#ifndef AUDIO_FEEDBACK_H
#define AUDIO_FEEDBACK_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Audio feedback patterns rendered via beep sequences.
 */
typedef enum {
    AUDIO_FEEDBACK_PATTERN_SINGLE = 0,  /**< One short confirmation beep. */
    AUDIO_FEEDBACK_PATTERN_DOUBLE = 1,  /**< Two short confirmation beeps. */
    AUDIO_FEEDBACK_PATTERN_TRIPLE = 2,  /**< Three short beeps (alert). */
} audio_feedback_pattern_t;

/**
 * @brief Render a short beep pattern through the speaker.
 *
 * @param pattern Pattern defining how many beeps to play.
 * @param allow_temp_driver When true the function will temporarily
 *        initialize and later tear down the audio driver if it is not
 *        already active. When false the call will fail if the driver is
 *        unavailable (useful inside voice-mode where audio is already
 *        configured).
 * @return ESP_OK on success or an error code otherwise.
 */
esp_err_t audio_feedback_play_pattern(audio_feedback_pattern_t pattern,
                                       bool allow_temp_driver);

/**
 * @brief Convenience helper for a single short beep.
 */
static inline esp_err_t audio_feedback_beep_single(bool allow_temp_driver) {
    return audio_feedback_play_pattern(AUDIO_FEEDBACK_PATTERN_SINGLE,
                                       allow_temp_driver);
}

/**
 * @brief Convenience helper for a double short beep.
 */
static inline esp_err_t audio_feedback_beep_double(bool allow_temp_driver) {
    return audio_feedback_play_pattern(AUDIO_FEEDBACK_PATTERN_DOUBLE,
                                       allow_temp_driver);
}

#ifdef __cplusplus
}
#endif

#endif // AUDIO_FEEDBACK_H

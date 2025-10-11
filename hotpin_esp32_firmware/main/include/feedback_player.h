/**
 * @file feedback_player.h
 * @brief Audio tone feedback generator for system state cues.
 */

#ifndef FEEDBACK_PLAYER_H
#define FEEDBACK_PLAYER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FEEDBACK_SOUND_BOOT = 0,
    FEEDBACK_SOUND_SHUTDOWN,
    FEEDBACK_SOUND_ERROR,
    FEEDBACK_SOUND_REC_START,
    FEEDBACK_SOUND_REC_STOP,
    FEEDBACK_SOUND_CAPTURE
} feedback_sound_t;

esp_err_t feedback_player_init(void);
esp_err_t feedback_player_play(feedback_sound_t sound);

#ifdef __cplusplus
}
#endif

#endif /* FEEDBACK_PLAYER_H */

/**
 * @file system_events.h
 * @brief Centralized system event definitions for the HotPin firmware.
 */

#ifndef SYSTEM_EVENTS_H
#define SYSTEM_EVENTS_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "websocket_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Button interaction event types produced by the button handler.
 */
typedef enum {
    BUTTON_EVENT_NONE = 0,
    BUTTON_EVENT_SINGLE_CLICK,
    BUTTON_EVENT_DOUBLE_CLICK,
    BUTTON_EVENT_LONG_PRESS,
    BUTTON_EVENT_LONG_PRESS_RELEASE
} button_event_type_t;

/**
 * @brief Envelope describing a button interaction.
 */
typedef struct {
    button_event_type_t type;
    uint32_t duration_ms;
} button_event_payload_t;

/**
 * @brief High-level system events consumed by the state manager FSM.
 */
typedef enum {
    SYSTEM_EVENT_NONE = 0,
    SYSTEM_EVENT_BOOT_COMPLETE,
    SYSTEM_EVENT_BUTTON_INPUT,
    SYSTEM_EVENT_WEBSOCKET_STATUS,
    SYSTEM_EVENT_CAPTURE_REQUEST,
    SYSTEM_EVENT_CAPTURE_COMPLETE,
    SYSTEM_EVENT_SHUTDOWN_REQUEST,
    SYSTEM_EVENT_ERROR_SIGNAL,
    SYSTEM_EVENT_STT_STARTED,
    SYSTEM_EVENT_STT_STOPPED,
    SYSTEM_EVENT_TTS_PLAYBACK_STARTED,
    SYSTEM_EVENT_TTS_PLAYBACK_FINISHED,
    SYSTEM_EVENT_PIPELINE_STAGE
} system_event_type_t;

/**
 * @brief Event payload dispatched through the central event queue.
 */
typedef struct {
    system_event_type_t type;
    uint32_t timestamp_ms;
    union {
        button_event_payload_t button;
        struct {
            websocket_status_t status;
        } websocket;
        struct {
            bool success;
            esp_err_t result;
        } capture;
        struct {
            esp_err_t code;
        } error;
        struct {
            websocket_pipeline_stage_t stage;
        } pipeline;
        struct {
            esp_err_t result;
        } tts;
    } data;
} system_event_t;

#ifdef __cplusplus
}
#endif

#endif // SYSTEM_EVENTS_H

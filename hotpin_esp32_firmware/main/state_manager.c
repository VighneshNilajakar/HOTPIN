/**
 * @file state_manager.c
 * @brief System state manager with mutex-protected driver switching
 * 
 * Implements finite state machine for camera/voice mode transitions:
 * - INIT → CAMERA_STANDBY → VOICE_ACTIVE ↔ CAMERA_STANDBY → SHUTDOWN
 * - Mutex-protected I2S/Camera driver switching
 * - Task coordination and error recovery
 */

#include "state_manager.h"
#include "config.h"
#include "camera_controller.h"
#include "audio_driver.h"
#include "feedback_player.h"
#include "stt_pipeline.h"
#include "tts_decoder.h"
#include "websocket_client.h"
#include "http_client.h"
#include "json_protocol.h"
#include "led_controller.h"
#include "event_dispatcher.h"
#include "system_events.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "inttypes.h"

static const char *TAG = TAG_STATE_MGR;
static system_state_t current_state = SYSTEM_STATE_INIT;
static system_state_t previous_state = SYSTEM_STATE_INIT;
static uint32_t s_mode_switch_count = 0;
static websocket_pipeline_stage_t s_pipeline_stage = WEBSOCKET_PIPELINE_STAGE_IDLE;
static bool s_transition_in_progress = false;
static bool s_capture_in_progress = false;
static bool s_tts_playback_active = false;
static bool s_transition_scheduled = false;

// Safe watchdog reset function to prevent errors when task is not registered
static inline void safe_task_wdt_reset(void) {
    esp_err_t ret = esp_task_wdt_reset();
    if (ret != ESP_OK) {
        // Only log at detailed level to avoid spam
        ESP_LOGD(TAG, "WDT reset failed: %s", esp_err_to_name(ret));
    }
}

// External synchronization primitive
extern SemaphoreHandle_t g_i2s_config_mutex;

// Task handles for coordination
// static TaskHandle_t camera_task_handle = NULL;  // Not currently used
// static TaskHandle_t stt_task_handle = NULL;     // Not currently used
// static TaskHandle_t tts_task_handle = NULL;     // Not currently used

// Transition timeout
#define STATE_TRANSITION_TIMEOUT_MS    5000
#define VOICE_PIPELINE_STAGE_WAIT_MS   20000
#define VOICE_PIPELINE_STAGE_GUARD_MS  1500
#define VOICE_TTS_FLUSH_WAIT_MS        5000

// ===========================
// Private Function Declarations
// ===========================
static esp_err_t transition_to_camera_mode(void);
static esp_err_t transition_to_voice_mode(void);
static esp_err_t handle_camera_capture(void);
static esp_err_t handle_shutdown(void);
static void handle_error_state(void);
static const char* state_to_string(system_state_t state);
static void wait_for_voice_pipeline_shutdown(void);
static esp_err_t capture_and_upload_image(void);
static void process_button_event(const button_event_payload_t *button_event);
static void process_websocket_status(websocket_status_t status);
static void execute_capture_sequence(void);
static void handle_pipeline_stage_event(websocket_pipeline_stage_t stage);
static void handle_stt_started(void);
static void handle_stt_stopped(void);
static void handle_tts_playback_started(void);
static void handle_tts_playback_finished(esp_err_t result);
static bool guardrails_is_pipeline_busy(void);
static bool guardrails_should_block_button(button_event_type_t type);
static bool guardrails_should_block_capture(void);
static void guardrails_signal_block(const char *reason);
static bool is_voice_pipeline_active(void);

// ===========================
// Public Functions
// ===========================

void state_manager_task(void *pvParameters) {
    ESP_LOGI(TAG, "State manager task started on Core %d", xPortGetCoreID());
    ESP_LOGI(TAG, "Priority: %d", uxTaskPriorityGet(NULL));
    
    // Initialize camera mode first
    ESP_LOGI(TAG, "Starting in camera mode...");
    current_state = SYSTEM_STATE_TRANSITIONING;
    s_transition_in_progress = true;
    esp_err_t init_ret = transition_to_camera_mode();
    s_transition_in_progress = false;

    if (init_ret == ESP_OK) {
        current_state = SYSTEM_STATE_CAMERA_STANDBY;
        ESP_LOGI(TAG, "✅ Entered CAMERA_STANDBY state");
        led_controller_set_state(LED_STATE_BREATHING);
    } else {
        ESP_LOGE(TAG, "❌ Failed to initialize camera - entering error state");
        current_state = SYSTEM_STATE_ERROR;
        esp_err_t beep_ret = feedback_player_play(FEEDBACK_SOUND_ERROR);
        if (beep_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to play error feedback: %s", esp_err_to_name(beep_ret));
        }
    }
    
    QueueHandle_t event_queue = event_dispatcher_queue();
    while (event_queue == NULL) {
        ESP_LOGW(TAG, "Waiting for event dispatcher queue...");
        vTaskDelay(pdMS_TO_TICKS(100));
        event_queue = event_dispatcher_queue();
    }

    system_event_t incoming_event;
    
    while (1) {
        // Reset watchdog timer
        safe_task_wdt_reset();
        
        if (xQueueReceive(event_queue, &incoming_event, pdMS_TO_TICKS(100)) == pdTRUE) {
            switch (incoming_event.type) {
                case SYSTEM_EVENT_BUTTON_INPUT:
                    process_button_event(&incoming_event.data.button);
                    break;
                case SYSTEM_EVENT_WEBSOCKET_STATUS:
                    process_websocket_status(incoming_event.data.websocket.status);
                    break;
                case SYSTEM_EVENT_CAPTURE_REQUEST:
                    ESP_LOGI(TAG, "Capture request received via event queue");
                    execute_capture_sequence();
                    break;
                case SYSTEM_EVENT_CAPTURE_COMPLETE:
                    ESP_LOGI(TAG, "Capture complete event: success=%d (%s)",
                             incoming_event.data.capture.success,
                             esp_err_to_name(incoming_event.data.capture.result));
                    break;
                case SYSTEM_EVENT_SHUTDOWN_REQUEST:
                    ESP_LOGW(TAG, "Shutdown requested via event queue");
                    current_state = SYSTEM_STATE_SHUTDOWN;
                    break;
                case SYSTEM_EVENT_ERROR_SIGNAL:
                    ESP_LOGE(TAG, "Error event received (code=%s)",
                             esp_err_to_name(incoming_event.data.error.code));
                    current_state = SYSTEM_STATE_ERROR;
                    break;
                case SYSTEM_EVENT_STT_STARTED:
                    handle_stt_started();
                    break;
                case SYSTEM_EVENT_STT_STOPPED:
                    handle_stt_stopped();
                    break;
                case SYSTEM_EVENT_TTS_PLAYBACK_STARTED:
                    handle_tts_playback_started();
                    break;
                case SYSTEM_EVENT_TTS_PLAYBACK_FINISHED:
                    handle_tts_playback_finished(incoming_event.data.tts.result);
                    break;
                case SYSTEM_EVENT_PIPELINE_STAGE:
                    handle_pipeline_stage_event(incoming_event.data.pipeline.stage);
                    break;
                case SYSTEM_EVENT_BOOT_COMPLETE:
                case SYSTEM_EVENT_NONE:
                default:
                    // No action required for these events yet
                    break;
            }
        }
        
        // FSM state-specific logic
        switch (current_state) {
            case SYSTEM_STATE_CAMERA_STANDBY:
                // Normal camera operation - no action needed
                break;
                
            case SYSTEM_STATE_VOICE_ACTIVE:
                // Voice interaction active - STT/TTS tasks running
                break;
                
            case SYSTEM_STATE_TRANSITIONING:
                // Transition in progress - handled in button event section
                ESP_LOGD(TAG, "Transitioning...");
                break;
                
            case SYSTEM_STATE_ERROR:
                handle_error_state();
                vTaskDelay(pdMS_TO_TICKS(1000));
                break;
                
            case SYSTEM_STATE_SHUTDOWN:
                ESP_LOGW(TAG, "Shutdown state reached");
                handle_shutdown();
                esp_task_wdt_delete(NULL);
                // Task will be deleted after shutdown
                vTaskDelete(NULL);
                break;
                
            default:
                ESP_LOGE(TAG, "Invalid state: %d", current_state);
                current_state = SYSTEM_STATE_ERROR;
                break;
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

system_state_t state_manager_get_state(void) {
    return current_state;
}

static bool is_voice_pipeline_active(void) {
    return websocket_client_is_pipeline_active() ||
           (s_pipeline_stage == WEBSOCKET_PIPELINE_STAGE_TTS) ||
           (s_pipeline_stage == WEBSOCKET_PIPELINE_STAGE_LLM) ||
           (s_pipeline_stage == WEBSOCKET_PIPELINE_STAGE_TRANSCRIPTION) ||
           s_tts_playback_active;
}

static bool guardrails_is_pipeline_busy(void) {
    return is_voice_pipeline_active();
}

static void guardrails_signal_block(const char *reason) {
    ESP_LOGW(TAG, "Guardrail blocked request: %s", reason ? reason : "unknown reason");
    esp_err_t beep_ret = feedback_player_play(FEEDBACK_SOUND_ERROR);
    if (beep_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to play error feedback: %s", esp_err_to_name(beep_ret));
    }
}

static bool guardrails_should_block_button(button_event_type_t type) {
    static uint32_t last_button_event_time = 0;
    static const uint32_t MIN_BUTTON_INTERVAL_MS = 500;  // Minimum 500ms between button events
    static const uint32_t MIN_VOICE_STATE_TRANSITION_DELAY_MS = 1000;  // Minimum delay for voice state transitions
    
    uint32_t current_time = (uint32_t)(esp_timer_get_time() / 1000ULL);
    uint32_t time_since_last = current_time - last_button_event_time;
    
    // Throttle button events to prevent rapid transitions
    if (time_since_last < MIN_BUTTON_INTERVAL_MS) {
        guardrails_signal_block("rapid button event");
        return true;
    }
    
    // Additional throttle for voice state transitions since they involve complex hardware setup
    if ((current_state == SYSTEM_STATE_VOICE_ACTIVE || 
         previous_state == SYSTEM_STATE_VOICE_ACTIVE) &&
        (type == BUTTON_EVENT_SINGLE_CLICK) && 
        time_since_last < MIN_VOICE_STATE_TRANSITION_DELAY_MS) {
        ESP_LOGW(TAG, "Throttling voice mode transition (elapsed: %u ms, required: %u ms)", 
                 (unsigned int)time_since_last, (unsigned int)MIN_VOICE_STATE_TRANSITION_DELAY_MS);
        guardrails_signal_block("voice mode transition too rapid");
        return true;
    }

    if (s_transition_in_progress) {
        guardrails_signal_block("state transition in progress");
        return true;
    }

    if (s_capture_in_progress) {
        guardrails_signal_block("camera capture in progress");
        return true;
    }

    // For voice active state, be more restrictive about transitions during pipeline operations
    if (current_state == SYSTEM_STATE_VOICE_ACTIVE && guardrails_is_pipeline_busy()) {
        if (type == BUTTON_EVENT_SINGLE_CLICK) {
            // Allow single click to stop voice pipeline but with warning
            ESP_LOGW(TAG, "Guardrail soft override: stopping voice pipeline while busy");
        } else if (type == BUTTON_EVENT_DOUBLE_CLICK) {
            // Block double click during voice pipeline activity
            guardrails_signal_block("audio pipeline busy - blocking capture");
            return true;
        }
    }

    // Block all transitions if pipeline is busy and not in voice active state
    if ((type == BUTTON_EVENT_SINGLE_CLICK || type == BUTTON_EVENT_DOUBLE_CLICK) && 
        guardrails_is_pipeline_busy() && 
        current_state != SYSTEM_STATE_VOICE_ACTIVE) {
        guardrails_signal_block("audio pipeline busy");
        return true;
    }
    
    // Update last button event time
    last_button_event_time = current_time;
    return false;
}

static bool guardrails_should_block_capture(void) {
    if (s_capture_in_progress) {
        guardrails_signal_block("camera capture already active");
        return true;
    }

    if (s_transition_in_progress) {
        guardrails_signal_block("state transition in progress");
        return true;
    }

    if (guardrails_is_pipeline_busy()) {
        guardrails_signal_block("audio pipeline busy");
        return true;
    }

    if (current_state == SYSTEM_STATE_TRANSITIONING) {
        guardrails_signal_block("FSM transitioning");
        return true;
    }

    return false;
}

static void process_button_event(const button_event_payload_t *button_event)
{
    if (button_event == NULL) {
        return;
    }

    ESP_LOGI(TAG, "Button event received: %d in state %s",
             button_event->type, state_to_string(current_state));

    // More robust check for transitioning state
    if (current_state == SYSTEM_STATE_TRANSITIONING || s_transition_in_progress) {
        ESP_LOGW(TAG, "Button event ignored - system transitioning");
        return;
    }

    // Enhanced guardrail check with detailed logging
    if (button_event->type != BUTTON_EVENT_LONG_PRESS && 
        button_event->type != BUTTON_EVENT_LONG_PRESS_RELEASE) {
        if (guardrails_should_block_button(button_event->type)) {
            return;
        }
    }

    switch (button_event->type) {
        case BUTTON_EVENT_SINGLE_CLICK:
            ESP_LOGI(TAG, "Single click - mode toggle requested");
            
            // Prevent button events during transitions to avoid conflicts
            if (current_state == SYSTEM_STATE_TRANSITIONING || s_transition_in_progress) {
                ESP_LOGW(TAG, "Button event ignored - system transitioning");
                return;
            }
            
            s_mode_switch_count++;

            if (current_state == SYSTEM_STATE_CAMERA_STANDBY) {
                ESP_LOGI(TAG, "Switching: Camera → Voice (count: %u)",
                         (unsigned int)s_mode_switch_count);
                previous_state = current_state;
                current_state = SYSTEM_STATE_TRANSITIONING;
                s_transition_in_progress = true;

                // Ensure all previous operations are completed before starting transition
                vTaskDelay(pdMS_TO_TICKS(50));
                
                esp_err_t voice_ret = transition_to_voice_mode();
                s_transition_in_progress = false;

                if (voice_ret == ESP_OK) {
                    current_state = SYSTEM_STATE_VOICE_ACTIVE;
                    ESP_LOGI(TAG, "✅ Entered VOICE_ACTIVE state");
                } else {
                    ESP_LOGE(TAG, "❌ Voice mode transition failed");
                    current_state = SYSTEM_STATE_ERROR;
                    esp_err_t beep_ret = feedback_player_play(FEEDBACK_SOUND_ERROR);
                    if (beep_ret != ESP_OK) {
                        ESP_LOGW(TAG, "Failed to play error feedback: %s", esp_err_to_name(beep_ret));
                    }
                }

            } else if (current_state == SYSTEM_STATE_VOICE_ACTIVE) {
                ESP_LOGI(TAG, "Switching: Voice → Camera (count: %u)",
                         (unsigned int)s_mode_switch_count);
                
                // Prevent button events during transitions to avoid conflicts
                if (s_transition_in_progress) {
                    ESP_LOGW(TAG, "Voice to camera transition ignored - system already transitioning");
                    return;
                }
                
                previous_state = current_state;
                current_state = SYSTEM_STATE_TRANSITIONING;
                s_transition_in_progress = true;

                // Ensure all previous operations are completed before starting transition
                vTaskDelay(pdMS_TO_TICKS(50));
                
                esp_err_t cam_ret = transition_to_camera_mode();
                s_transition_in_progress = false;

                if (cam_ret == ESP_OK) {
                    current_state = SYSTEM_STATE_CAMERA_STANDBY;
                    ESP_LOGI(TAG, "✅ Entered CAMERA_STANDBY state");
                    // Reset transition scheduled flag since transition completed
                    s_transition_scheduled = false;
                } else {
                    ESP_LOGE(TAG, "❌ Camera mode transition failed");
                    current_state = SYSTEM_STATE_ERROR;
                    // Reset transition scheduled flag since transition failed
                    s_transition_scheduled = false;
                    esp_err_t beep_ret = feedback_player_play(FEEDBACK_SOUND_ERROR);
                    if (beep_ret != ESP_OK) {
                        ESP_LOGW(TAG, "Failed to play error feedback: %s", esp_err_to_name(beep_ret));
                    }
                }
            } else {
                ESP_LOGW(TAG, "Single click received in state %s - no action",
                         state_to_string(current_state));
            }
            break;

        case BUTTON_EVENT_DOUBLE_CLICK:
            ESP_LOGI(TAG, "Double-click detected - triggering capture sequence");
            execute_capture_sequence();
            break;

        case BUTTON_EVENT_LONG_PRESS:
            ESP_LOGW(TAG, "Long press - shutdown requested");
            current_state = SYSTEM_STATE_SHUTDOWN;
            break;

        case BUTTON_EVENT_LONG_PRESS_RELEASE:
            ESP_LOGI(TAG, "Long press released after %u ms",
                     (unsigned int)button_event->duration_ms);
            break;

        case BUTTON_EVENT_NONE:
        default:
            ESP_LOGW(TAG, "Unhandled button event type: %d", button_event->type);
            break;
    }
}

static void execute_capture_sequence(void)
{
    if (guardrails_should_block_capture()) {
        ESP_LOGW(TAG, "Capture request ignored by guardrail");
        return;
    }

    esp_err_t ret = handle_camera_capture();
    if (ret == ESP_OK) {
        return;
    }

    ESP_LOGE(TAG, "Camera capture sequence failed (%s)", esp_err_to_name(ret));
    led_controller_set_state(LED_STATE_SOS);
    vTaskDelay(pdMS_TO_TICKS(2000));
    led_state_t recovery_state =
        (current_state == SYSTEM_STATE_VOICE_ACTIVE) ?
        LED_STATE_SOLID : LED_STATE_BREATHING;
    led_controller_set_state(recovery_state);
}

static void process_websocket_status(websocket_status_t status)
{
    switch (status) {
        case WEBSOCKET_STATUS_CONNECTED:
            ESP_LOGI(TAG, "WebSocket connected");
            if (current_state == SYSTEM_STATE_CAMERA_STANDBY) {
                led_controller_set_state(LED_STATE_BREATHING);
            }
            break;
        case WEBSOCKET_STATUS_DISCONNECTED:
            ESP_LOGW(TAG, "WebSocket disconnected");
            led_controller_set_state(LED_STATE_PULSING);
            
            // CRITICAL FIX: Handle disconnection better in voice mode
            // Prevent transitions during active transitions to avoid conflicts
            if (current_state == SYSTEM_STATE_VOICE_ACTIVE && !s_transition_in_progress) {
                // Only schedule transition if one isn't already scheduled to prevent multiple transitions
                if (!s_transition_scheduled) {
                    s_transition_scheduled = true;
                    ESP_LOGW(TAG, "WebSocket disconnected while in voice mode - triggering transition to camera");
                    // Schedule a transition to camera mode to prevent hanging
                    system_event_t evt = {
                        .type = SYSTEM_EVENT_WEBSOCKET_STATUS,
                        .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL),
                        .data.websocket = {
                            .status = WEBSOCKET_STATUS_DISCONNECTED,
                        }
                    };
                    if (!event_dispatcher_post(&evt, pdMS_TO_TICKS(100))) {
                        ESP_LOGE(TAG, "Failed to enqueue WebSocket disconnect event for voice mode");
                        s_transition_scheduled = false; // Reset the flag if posting failed
                    }
                } else {
                    ESP_LOGD(TAG, "WebSocket disconnect transition already scheduled, skipping");
                }
            } else if (current_state == SYSTEM_STATE_VOICE_ACTIVE && s_transition_in_progress) {
                ESP_LOGW(TAG, "WebSocket disconnect ignored - system transitioning");
            }
            break;
        case WEBSOCKET_STATUS_ERROR:
            ESP_LOGE(TAG, "WebSocket error signalled");
            led_controller_set_state(LED_STATE_SOS);
            
            // CRITICAL FIX: Handle error better in voice mode
            // Prevent transitions during active transitions to avoid conflicts
            if (current_state == SYSTEM_STATE_VOICE_ACTIVE && !s_transition_in_progress) {
                // Only schedule transition if one isn't already scheduled to prevent multiple transitions
                if (!s_transition_scheduled) {
                    s_transition_scheduled = true;
                    ESP_LOGE(TAG, "WebSocket error occurred while in voice mode - triggering transition to camera");
                    // Schedule a transition to camera mode to prevent hanging
                    system_event_t evt = {
                        .type = SYSTEM_EVENT_WEBSOCKET_STATUS,
                        .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL),
                        .data.websocket = {
                            .status = WEBSOCKET_STATUS_ERROR,
                        }
                    };
                    if (!event_dispatcher_post(&evt, pdMS_TO_TICKS(100))) {
                        ESP_LOGE(TAG, "Failed to enqueue WebSocket error event for voice mode");
                        s_transition_scheduled = false; // Reset the flag if posting failed
                    }
                } else {
                    ESP_LOGD(TAG, "WebSocket error transition already scheduled, skipping");
                }
            } else if (current_state == SYSTEM_STATE_VOICE_ACTIVE && s_transition_in_progress) {
                ESP_LOGW(TAG, "WebSocket error ignored - system transitioning");
            }
            break;
        default:
            break;
    }
}

static void handle_pipeline_stage_event(websocket_pipeline_stage_t stage)
{
    s_pipeline_stage = stage;
    ESP_LOGI(TAG, "Pipeline stage event: %s",
             websocket_client_pipeline_stage_to_string(stage));

    if (current_state != SYSTEM_STATE_VOICE_ACTIVE) {
        return;
    }

    switch (stage) {
        case WEBSOCKET_PIPELINE_STAGE_TRANSCRIPTION:
        case WEBSOCKET_PIPELINE_STAGE_LLM:
            led_controller_set_state(LED_STATE_PULSING);
            break;
        case WEBSOCKET_PIPELINE_STAGE_TTS:
            led_controller_set_state(LED_STATE_SOLID);
            break;
        case WEBSOCKET_PIPELINE_STAGE_COMPLETE:
            tts_decoder_notify_end_of_stream();
            led_controller_set_state(LED_STATE_SOLID);
            break;
        case WEBSOCKET_PIPELINE_STAGE_IDLE:
            // When entering IDLE state, ensure TTS is fully reset for next session
            if (s_pipeline_stage == WEBSOCKET_PIPELINE_STAGE_COMPLETE) {
                // Only reset if we're coming from complete state
                esp_err_t reset_ret = tts_decoder_flush_and_reset();
                if (reset_ret != ESP_OK) {
                    ESP_LOGW(TAG, "TTS flush and reset on IDLE transition failed: %s", esp_err_to_name(reset_ret));
                }
            }
            led_controller_set_state(LED_STATE_SOLID);
            break;
        default:
            break;
    }
}

static void handle_stt_started(void)
{
    ESP_LOGI(TAG, "STT pipeline reported start");
    if (current_state == SYSTEM_STATE_VOICE_ACTIVE) {
        led_controller_set_state(LED_STATE_SOLID);
    }
}

static void handle_stt_stopped(void)
{
    ESP_LOGI(TAG, "STT pipeline reported stop");
    if (current_state == SYSTEM_STATE_VOICE_ACTIVE) {
        led_controller_set_state(LED_STATE_SOLID);
    }
}

static void handle_tts_playback_started(void)
{
    ESP_LOGI(TAG, "TTS playback start event received");
    s_tts_playback_active = true;
    if (current_state == SYSTEM_STATE_VOICE_ACTIVE) {
        led_controller_set_state(LED_STATE_SOLID);
    }
}

static void handle_tts_playback_finished(esp_err_t result)
{
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "TTS playback finished successfully");
    } else {
        ESP_LOGE(TAG, "TTS playback finished with error: %s", esp_err_to_name(result));
    }

    s_tts_playback_active = false;

    if (current_state == SYSTEM_STATE_VOICE_ACTIVE) {
        led_controller_set_state(LED_STATE_SOLID);
    }
}

// ===========================
// Private Functions
// ===========================

static void wait_for_voice_pipeline_shutdown(void) {
    TickType_t guard_start = xTaskGetTickCount();
    const TickType_t guard_timeout = pdMS_TO_TICKS(VOICE_PIPELINE_STAGE_GUARD_MS);

    // Guard window: allow the server to transition into an active stage after EOS
    while (!websocket_client_is_pipeline_active()) {
        websocket_pipeline_stage_t stage = websocket_client_get_pipeline_stage();
        if (stage == WEBSOCKET_PIPELINE_STAGE_COMPLETE) {
            break;  // Complete, can proceed
        }

        if ((xTaskGetTickCount() - guard_start) >= guard_timeout) {
            break;
        }

        safe_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    const TickType_t overall_start = xTaskGetTickCount();
    const TickType_t overall_timeout = pdMS_TO_TICKS(VOICE_PIPELINE_STAGE_WAIT_MS * 3); // Tripled timeout for more stability (increased from 2x)

    while (true) {
        websocket_pipeline_stage_t stage = websocket_client_get_pipeline_stage();
        bool pipeline_active = websocket_client_is_pipeline_active();

        if (!pipeline_active && stage != WEBSOCKET_PIPELINE_STAGE_TTS) {
            // Pipeline is truly idle (not in TTS stage)
            if (stage == WEBSOCKET_PIPELINE_STAGE_COMPLETE) {
                ESP_LOGI(TAG, "Voice pipeline reported COMPLETE");
            } else {
                ESP_LOGI(TAG, "Voice pipeline became idle at stage %s",
                         websocket_client_pipeline_stage_to_string(stage));
            }
            break;
        }

        if ((xTaskGetTickCount() - overall_start) >= overall_timeout) {
            ESP_LOGW(TAG, "Voice pipeline still active (%s) after %u ms",
                     websocket_client_pipeline_stage_to_string(stage),
                     (unsigned int)(VOICE_PIPELINE_STAGE_WAIT_MS * 3)); // Updated timeout
            break;
        }

        safe_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // Wait for TTS playback to complete before proceeding
    // CRITICAL FIX: More robust TTS drain handling with better timeout management
    if (tts_decoder_has_pending_audio() || s_tts_playback_active) {
        size_t pending_bytes = tts_decoder_get_pending_bytes();
        ESP_LOGI(TAG, "Waiting for TTS playback drain (~%zu bytes pending, timeout %u ms)",
                 pending_bytes, (unsigned int)(VOICE_TTS_FLUSH_WAIT_MS * 3)); // Tripled timeout (increased from 2x)

        // Wait in smaller increments and check for state changes
        const TickType_t tts_start = xTaskGetTickCount();
        const TickType_t tts_timeout = pdMS_TO_TICKS(VOICE_TTS_FLUSH_WAIT_MS * 3); // Tripled timeout
        uint32_t drain_checks = 0;
        uint32_t timeout_warnings = 0;
        
        while (true) {
            // ✅ FIX #3: Pre-check - if TTS is already idle, exit immediately without waiting
            // This catches the case where playback completed before we entered this loop
            if (!tts_decoder_is_playing() && !tts_decoder_has_pending_audio()) {
                ESP_LOGI(TAG, "TTS playback already finished - no need to wait");
                break;
            }
            
            // Wait for TTS decoder to become idle with a 200ms timeout
            esp_err_t wait_ret = tts_decoder_wait_for_idle(200);
            
            // ✅ FIX #3: If wait returns ESP_OK, break IMMEDIATELY
            // The wait_for_idle() function has already confirmed the task exited cleanly
            if (wait_ret == ESP_OK) {
                ESP_LOGI(TAG, "TTS decoder confirmed idle");
                break;
            }
            
            websocket_pipeline_stage_t current_stage = websocket_client_get_pipeline_stage();
            bool pipeline_still_active = websocket_client_is_pipeline_active();
            
            // CRITICAL FIX: Better handling of timeout conditions
            if (wait_ret == ESP_ERR_TIMEOUT) {
                timeout_warnings++;
                ESP_LOGW(TAG, "TTS wait for idle timeout #%u (drain_checks=%u)", 
                         (unsigned int)timeout_warnings, (unsigned int)drain_checks);
                
                // After multiple timeouts, we might need to force completion
                if (timeout_warnings > 10) { // Increased from 6 to 10 warnings
                    ESP_LOGW(TAG, "Multiple TTS idle timeouts - checking if we can proceed anyway");
                    
                    // If the pipeline is complete and no pending audio, we can continue
                    if (current_stage == WEBSOCKET_PIPELINE_STAGE_COMPLETE && 
                        !pipeline_still_active && 
                        !tts_decoder_has_pending_audio()) {
                        ESP_LOGI(TAG, "Pipeline complete despite timeouts - proceeding with shutdown");
                        break;
                    }
                }
            }
            
            // Check if TTS has completed or if there's no more pending audio
            if (!tts_decoder_has_pending_audio() && !s_tts_playback_active) {
                ESP_LOGI(TAG, "TTS playback drained successfully after %u checks (and %u timeouts)", 
                         (unsigned int)drain_checks, (unsigned int)timeout_warnings);
                break;
            }
            
            // Check for overall timeout
            if ((xTaskGetTickCount() - tts_start) >= tts_timeout) {
                ESP_LOGW(TAG, "TTS playback drain timed out after %u checks and %u timeouts; proceeding with forced shutdown", 
                         (unsigned int)drain_checks, (unsigned int)timeout_warnings);
                // CRITICAL: Force stop TTS decoder to prevent hanging, but more gracefully
                // Instead of immediately stopping, try to flush first
                esp_err_t flush_ret = tts_decoder_flush_and_reset();
                if (flush_ret != ESP_OK) {
                    ESP_LOGW(TAG, "TTS flush and reset failed: %s - forcing stop", esp_err_to_name(flush_ret));
                    tts_decoder_stop();
                }
                break;
            }
            
            // If the pipeline has moved to complete state and no audio is pending, we can continue
            if (current_stage == WEBSOCKET_PIPELINE_STAGE_COMPLETE && 
                !pipeline_still_active && 
                !tts_decoder_has_pending_audio()) {
                ESP_LOGI(TAG, "TTS drain complete - pipeline finished after %u checks", (unsigned int)drain_checks);
                break;
            }
            
            drain_checks++;
            safe_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(100)); // Increased from 50ms to 100ms
        }
    } else {
        ESP_LOGI(TAG, "TTS playback already drained - no pending audio");
    }
    
    // CRITICAL FIX: Ensure TTS decoder is in a completely clean state
    ESP_LOGI(TAG, "Flushing and resetting TTS decoder for next session");
    esp_err_t flush_ret = tts_decoder_flush_and_reset();
    if (flush_ret != ESP_OK) {
        ESP_LOGW(TAG, "TTS flush and reset failed: %s - forcing additional cleanup", esp_err_to_name(flush_ret));
        // Additional safety measure: stop TTS decoder if flush failed
        tts_decoder_stop();
        // Reset session manually
        tts_decoder_reset_session();
    }
    
    // CRITICAL FIX: Additional safety check: ensure all audio components are truly idle before proceeding
    // This prevents race conditions where audio might still be processing
    vTaskDelay(pdMS_TO_TICKS(200)); // Extra 200ms delay to ensure audio processing completes
    
    // Final check: ensure TTS decoder is really stopped
    if (tts_decoder_is_playing()) {
        ESP_LOGW(TAG, "TTS decoder still playing after shutdown - forcing stop");
        tts_decoder_stop();
    }
}

static esp_err_t transition_to_camera_mode(void) {
    ESP_LOGI(TAG, "=== TRANSITION TO CAMERA MODE ===");
    
    esp_err_t ret = ESP_OK;
    bool audio_was_initialized = audio_driver_is_initialized();
    
    // Step 1: Stop voice mode components if active
    if (previous_state == SYSTEM_STATE_VOICE_ACTIVE) {
        ESP_LOGI(TAG, "Stopping voice mode components...");
        
        // Stop STT pipeline first, then wait for all voice pipeline operations to complete
        stt_pipeline_stop();
        
        // Wait for the complete voice pipeline shutdown, including TTS playback
        wait_for_voice_pipeline_shutdown();
        
        // Now it's safe to stop the TTS decoder after all audio has been processed
        tts_decoder_stop();
        s_tts_playback_active = false;
        s_pipeline_stage = WEBSOCKET_PIPELINE_STAGE_IDLE;

        // Small delay for tasks to finish
        vTaskDelay(pdMS_TO_TICKS(100));

        audio_was_initialized = audio_driver_is_initialized();
    }
    
    // Step 2: Acquire I2S mutex (CRITICAL SECTION)
    ESP_LOGI(TAG, "Acquiring I2S mutex...");
    if (xSemaphoreTake(g_i2s_config_mutex, pdMS_TO_TICKS(STATE_TRANSITION_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire I2S mutex - timeout");
        return ESP_ERR_TIMEOUT;
    }
    
    // Step 3: Deinitialize audio drivers if active
    if (audio_was_initialized) {
        ESP_LOGI(TAG, "Deinitializing audio drivers...");
        ret = audio_driver_deinit();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to deinit audio: %s", esp_err_to_name(ret));
            xSemaphoreGive(g_i2s_config_mutex);
            return ret;
        }
        // Ensure audio driver state is updated after deinit
        vTaskDelay(pdMS_TO_TICKS(50));  // Brief stabilization delay
    } else {
        ESP_LOGI(TAG, "Audio drivers already inactive; skipping deinit");
    }
    
    // Step 4: Initialize camera
    ESP_LOGI(TAG, "Initializing camera...");
    ret = camera_controller_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init camera: %s", esp_err_to_name(ret));
        xSemaphoreGive(g_i2s_config_mutex);
        return ret;
    }
    
    // Step 5: Release I2S mutex
    xSemaphoreGive(g_i2s_config_mutex);
    ESP_LOGI(TAG, "I2S mutex released");
    
    ESP_LOGI(TAG, "✅ Camera mode transition complete");

    if (previous_state == SYSTEM_STATE_VOICE_ACTIVE) {
        esp_err_t stop_sound_ret = feedback_player_play(FEEDBACK_SOUND_REC_STOP);
        if (stop_sound_ret != ESP_OK) {
            ESP_LOGW(TAG, "Voice stop feedback failed: %s", esp_err_to_name(stop_sound_ret));
        }
        led_controller_set_state(LED_STATE_BREATHING);
    }

    return ESP_OK;
}

static esp_err_t capture_and_upload_image(void) {
    ESP_LOGI(TAG, "Capturing frame from camera");

    // Check if audio drivers are active and deinitialize them temporarily for better camera performance
    bool audio_was_initialized = audio_driver_is_initialized();
    if (audio_was_initialized) {
        ESP_LOGI(TAG, "Temporarily deinit audio drivers for camera capture");
        audio_driver_deinit();
    }

    camera_fb_t *fb = camera_controller_capture_frame();
    if (fb == NULL) {
        ESP_LOGE(TAG, "Frame capture failed");

        // Reinitialize audio if it was active before capture
        if (audio_was_initialized) {
            audio_driver_init();
        }
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Frame captured: %zu bytes", fb->len);

    char session_id[64];
    json_protocol_generate_session_id(session_id, sizeof(session_id));

    ESP_LOGI(TAG, "Uploading image using session %s", session_id);
    char response[512];
    esp_err_t ret = http_client_upload_image(session_id, fb->buf, fb->len,
                                             response, sizeof(response));

    esp_camera_fb_return(fb);

    // Reinitialize audio if it was active before capture
    if (audio_was_initialized) {
        esp_err_t audio_ret = audio_driver_init();
        if (audio_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to reinitialize audio after capture: %s", esp_err_to_name(audio_ret));
        }
    }

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Image uploaded successfully");
    } else {
        ESP_LOGE(TAG, "Image upload failed: %s", esp_err_to_name(ret));
    }

    system_event_t evt = {
        .type = SYSTEM_EVENT_CAPTURE_COMPLETE,
        .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL),
        .data.capture = {
            .success = (ret == ESP_OK),
            .result = ret,
        },
    };
    if (!event_dispatcher_post(&evt, pdMS_TO_TICKS(10))) {
        ESP_LOGW(TAG, "Failed to enqueue capture completion event");
    }

    return ret;
}

static esp_err_t handle_camera_capture(void) {
    ESP_LOGI(TAG, "Starting camera capture sequence");

    if (s_capture_in_progress) {
        ESP_LOGW(TAG, "Camera capture already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    if (current_state == SYSTEM_STATE_VOICE_ACTIVE) {
        ESP_LOGW(TAG, "Camera capture not allowed while voice mode is active");
        return ESP_ERR_INVALID_STATE;
    }

    s_capture_in_progress = true;

    led_controller_set_state(LED_STATE_FLASH);
    esp_err_t capture_sound_ret = feedback_player_play(FEEDBACK_SOUND_CAPTURE);
    if (capture_sound_ret != ESP_OK) {
        ESP_LOGW(TAG, "Capture sound playback failed: %s", esp_err_to_name(capture_sound_ret));
    }

    bool capture_success = false;
    esp_err_t ret = ESP_OK;
    bool camera_initialized_here = false;
    bool audio_was_active_before_capture = false;

    if (current_state != SYSTEM_STATE_CAMERA_STANDBY) {
        ESP_LOGW(TAG, "Camera capture requested during %s", state_to_string(current_state));
    }

    // Check if audio drivers are active and temporarily disable them during capture
    // This can improve camera capture stability since both use DMA resources
    audio_was_active_before_capture = audio_driver_is_initialized();

    if (!camera_controller_is_initialized()) {
        ESP_LOGI(TAG, "Camera not initialized; attempting setup before capture");
        
        // If audio was active, we might need to deinitialize it to free up GPIO/ISR resources
        if (audio_was_active_before_capture) {
            ESP_LOGI(TAG, "Audio drivers active, deinit for camera setup");
            audio_driver_deinit();
        }
        
        ret = camera_controller_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize camera: %s", esp_err_to_name(ret));
            
            // Reinitialize audio if it was active before capture attempt
            if (audio_was_active_before_capture) {
                esp_err_t audio_ret = audio_driver_init();
                if (audio_ret != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to reinitialize audio after failed capture: %s", esp_err_to_name(audio_ret));
                }
            }
            goto finalize_capture;
        }
        camera_initialized_here = true;
    }

    ret = capture_and_upload_image();
    capture_success = (ret == ESP_OK);

finalize_capture:
    if (camera_initialized_here) {
        ESP_LOGI(TAG, "Deinitializing temporary camera session after capture");
        camera_controller_deinit();
    }

    // Restore audio if it was active before capture
    if (audio_was_active_before_capture && !camera_initialized_here && current_state == SYSTEM_STATE_CAMERA_STANDBY) {
        // Only reinitialize audio if we didn't fully deinit the camera
        // and we're staying in camera mode (not transitioning)
        esp_err_t audio_ret = audio_driver_init();
        if (audio_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to reinitialize audio after capture: %s", esp_err_to_name(audio_ret));
        }
    }

    if (capture_success) {
        led_state_t next_led = (current_state == SYSTEM_STATE_VOICE_ACTIVE) ?
                               LED_STATE_SOLID : LED_STATE_BREATHING;
        led_controller_set_state(next_led);
    } else {
        esp_err_t beep_ret = feedback_player_play(FEEDBACK_SOUND_ERROR);
        if (beep_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to play error feedback: %s", esp_err_to_name(beep_ret));
        }
        led_controller_set_state(LED_STATE_BREATHING);
    }

    s_capture_in_progress = false;

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Camera capture sequence complete");
    }

    return ret;
}

static esp_err_t transition_to_voice_mode(void) {
    ESP_LOGI(TAG, "=== TRANSITION TO VOICE MODE ===");
    
    esp_err_t ret = ESP_OK;
    
    // Step 1: Stop camera if active
    if (previous_state == SYSTEM_STATE_CAMERA_STANDBY) {
        ESP_LOGI(TAG, "Stopping camera...");
        // Camera tasks will be suspended during transition
    }
    
    // Step 2: Acquire I2S mutex (CRITICAL SECTION)
    ESP_LOGI(TAG, "╔══════════════════════════════════════════════════");
    ESP_LOGI(TAG, "║ STEP 2: Acquiring I2S configuration mutex");
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════════");
    ESP_LOGI(TAG, "  Timeout: %d ms", STATE_TRANSITION_TIMEOUT_MS);
    ESP_LOGI(TAG, "  Timestamp: %lld ms", (long long)(esp_timer_get_time() / 1000));
    
    int64_t mutex_start = esp_timer_get_time();
    if (xSemaphoreTake(g_i2s_config_mutex, pdMS_TO_TICKS(STATE_TRANSITION_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "❌ Failed to acquire I2S mutex - timeout after %lld ms", 
                 (long long)((esp_timer_get_time() - mutex_start) / 1000));
        return ESP_ERR_TIMEOUT;
    }
    int64_t mutex_time = (esp_timer_get_time() - mutex_start) / 1000;
    ESP_LOGI(TAG, "  ✓ Mutex acquired (took %"PRIu32" ms)", (uint32_t)mutex_time);
    
    // Step 3: Deinitialize camera
    ESP_LOGI(TAG, "╔══════════════════════════════════════════════════");
    ESP_LOGI(TAG, "║ STEP 3: Deinitializing camera hardware");
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════════");
    ESP_LOGI(TAG, "  Free heap before: %u bytes", (unsigned int)esp_get_free_heap_size());
    ESP_LOGI(TAG, "  Free PSRAM before: %u bytes", (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    
    int64_t cam_deinit_start = esp_timer_get_time();
    ret = camera_controller_deinit();
    int64_t cam_deinit_time = (esp_timer_get_time() - cam_deinit_start) / 1000;
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ Failed to deinit camera: %s (took %"PRIu32" ms)", 
                 esp_err_to_name(ret), (uint32_t)cam_deinit_time);
        xSemaphoreGive(g_i2s_config_mutex);
        return ret;
    }
    ESP_LOGI(TAG, "  ✓ Camera deinitialized (took %"PRIu32" ms)", (uint32_t)cam_deinit_time);
    ESP_LOGI(TAG, "  Free heap after: %u bytes", (unsigned int)esp_get_free_heap_size());
    ESP_LOGI(TAG, "  Free PSRAM after: %u bytes", (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    
    // CRITICAL: Extended delay to allow camera interrupt resources to be fully released
    ESP_LOGI(TAG, "╔══════════════════════════════════════════════════");
    ESP_LOGI(TAG, "║ HARDWARE STABILIZATION - CRITICAL");
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════════");
    ESP_LOGI(TAG, "  Phase 1: Initial settle (100ms) - Free camera interrupts");
    vTaskDelay(pdMS_TO_TICKS(100));
    
    ESP_LOGI(TAG, "  Phase 2: GPIO matrix settle (100ms) - Reconfigure pins");
    vTaskDelay(pdMS_TO_TICKS(100));
    
    ESP_LOGI(TAG, "  Phase 3: Final settle (50ms) - Stabilize state");
    vTaskDelay(pdMS_TO_TICKS(50));
    
    ESP_LOGI(TAG, "  ✓ Total stabilization: 250ms");
    ESP_LOGI(TAG, "  Timestamp: %lld ms", (long long)(esp_timer_get_time() / 1000));
    
    // Step 4: Initialize audio drivers (I2S0 full-duplex)
    ESP_LOGI(TAG, "╔══════════════════════════════════════════════════");
    ESP_LOGI(TAG, "║ STEP 4: Initializing I2S audio drivers");
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════════");
    ESP_LOGI(TAG, "  Free heap before: %u bytes", (unsigned int)esp_get_free_heap_size());
    
    int64_t audio_init_start = esp_timer_get_time();
    ret = audio_driver_init();
    int64_t audio_init_time = (esp_timer_get_time() - audio_init_start) / 1000;
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ Failed to init audio: %s (took %"PRIu32" ms)", 
                 esp_err_to_name(ret), (uint32_t)audio_init_time);
        ESP_LOGE(TAG, "  Free heap at failure: %u bytes", (unsigned int)esp_get_free_heap_size());
        
        // Attempt recovery: reinitialize camera
        ESP_LOGW(TAG, "⚠ Attempting recovery - reinitializing camera");
        camera_controller_init();
        
        xSemaphoreGive(g_i2s_config_mutex);
        return ret;
    }
    ESP_LOGI(TAG, "  ✓ Audio initialized (took %"PRIu32" ms)", (uint32_t)audio_init_time);
    ESP_LOGI(TAG, "  Free heap after: %u bytes", (unsigned int)esp_get_free_heap_size());
    
    // Step 5: Release I2S mutex
    xSemaphoreGive(g_i2s_config_mutex);
    ESP_LOGI(TAG, "╔══════════════════════════════════════════════════");
    ESP_LOGI(TAG, "║ STEP 5: I2S mutex released");
    ESP_LOGI(TAG, "║ Total transition time: %"PRIu32" ms", 
             (uint32_t)(mutex_time + cam_deinit_time + 250 + audio_init_time));
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════════");
    
    // Step 6: Start STT and TTS pipelines with additional error handling
    ESP_LOGI(TAG, "╔══════════════════════════════════════════════════");
    ESP_LOGI(TAG, "║ STEP 6: Starting STT/TTS pipelines");
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════════");

    // Ensure clean state for the TTS decoder before starting
    ret = tts_decoder_flush_and_reset();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "TTS flush and reset before start failed: %s", esp_err_to_name(ret));
    }
    
    ret = stt_pipeline_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start STT pipeline: %s", esp_err_to_name(ret));
    }

    // Brief stagger prevents simultaneous startup logs from contending on UART
    vTaskDelay(pdMS_TO_TICKS(50));
    
    ret = tts_decoder_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start TTS decoder: %s", esp_err_to_name(ret));
    }
    
    // Add safety delay to allow pipeline initialization
    vTaskDelay(pdMS_TO_TICKS(50));
    
    esp_err_t start_sound_ret = feedback_player_play(FEEDBACK_SOUND_REC_START);
    if (start_sound_ret != ESP_OK) {
        ESP_LOGW(TAG, "Voice start feedback failed: %s", esp_err_to_name(start_sound_ret));
    }
    led_controller_set_state(LED_STATE_SOLID);
    
    ESP_LOGI(TAG, "✅ Voice mode transition complete");
    return ESP_OK;
}

static esp_err_t handle_shutdown(void) {
    ESP_LOGW(TAG, "=== SYSTEM SHUTDOWN ===");
    
    // Play shutdown feedback first before deinitializing audio
    esp_err_t shutdown_sound_ret = feedback_player_play(FEEDBACK_SOUND_SHUTDOWN);
    if (shutdown_sound_ret != ESP_OK) {
        ESP_LOGW(TAG, "Shutdown feedback failed: %s", esp_err_to_name(shutdown_sound_ret));
    }
    led_controller_set_state(LED_STATE_SOLID);
    vTaskDelay(pdMS_TO_TICKS(600));
    led_controller_set_state(LED_STATE_BREATHING);
    vTaskDelay(pdMS_TO_TICKS(1200));
    led_controller_set_state(LED_STATE_OFF);

    // Stop all ongoing operations
    ESP_LOGI(TAG, "Stopping all subsystems...");
    
    // Acquire mutex for safe shutdown
    if (xSemaphoreTake(g_i2s_config_mutex, pdMS_TO_TICKS(5000)) == pdTRUE) {
        
        // Deinitialize audio if active
        if (audio_driver_is_initialized()) {
            ESP_LOGI(TAG, "Shutting down audio...");
            audio_driver_deinit();
        }
        
        // Deinitialize camera
        ESP_LOGI(TAG, "Shutting down camera...");
        camera_controller_deinit();
        
        xSemaphoreGive(g_i2s_config_mutex);
    }
    
    // Stop STT/TTS
    stt_pipeline_stop();
    tts_decoder_stop();
    
    ESP_LOGI(TAG, "Stopping WebSocket client");
    if (websocket_client_is_connected()) {
        esp_err_t ws_ret = websocket_client_disconnect();
        if (ws_ret != ESP_OK) {
            ESP_LOGW(TAG, "WebSocket disconnect returned %s", esp_err_to_name(ws_ret));
        }
    }
    websocket_client_force_stop();
    
    ESP_LOGI(TAG, "✅ Shutdown complete");
    return ESP_OK;
}

static void handle_error_state(void) {
    ESP_LOGE(TAG, "System in ERROR state (previous: %s)", state_to_string(previous_state));
    
    // Attempt recovery based on previous state
    static uint32_t error_count = 0;
    static uint32_t last_signaled_error = 0;
    error_count++;

    if (error_count != last_signaled_error) {
        led_controller_set_state(LED_STATE_SOS);
        esp_err_t error_sound_ret = feedback_player_play(FEEDBACK_SOUND_ERROR);
        if (error_sound_ret != ESP_OK) {
            ESP_LOGW(TAG, "Error feedback failed: %s", esp_err_to_name(error_sound_ret));
        }
        last_signaled_error = error_count;
    }
    
    if (error_count > 3) {
        ESP_LOGE(TAG, "Too many errors (%u) - entering shutdown", (unsigned int)error_count);
        current_state = SYSTEM_STATE_SHUTDOWN;
        return;
    }
    
    ESP_LOGW(TAG, "Attempting recovery (attempt %u/3)...", (unsigned int)error_count);
    
    // Try to return to camera mode as safe fallback
    current_state = SYSTEM_STATE_TRANSITIONING;
    s_transition_in_progress = true;
    esp_err_t recovery_ret = transition_to_camera_mode();
    s_transition_in_progress = false;
    if (recovery_ret == ESP_OK) {
        current_state = SYSTEM_STATE_CAMERA_STANDBY;
        error_count = 0;  // Reset error count on success
        last_signaled_error = 0;
        // Reset transition scheduled flag since recovery transition completed
        s_transition_scheduled = false;
        ESP_LOGI(TAG, "✅ Recovery successful - back to camera mode");
    } else {
        current_state = SYSTEM_STATE_ERROR;
        // Reset transition scheduled flag since recovery failed
        s_transition_scheduled = false;
        ESP_LOGE(TAG, "❌ Recovery failed");
    }
}

static const char* state_to_string(system_state_t state) {
    switch (state) {
        case SYSTEM_STATE_INIT:            return "INIT";
        case SYSTEM_STATE_CAMERA_STANDBY:  return "CAMERA_STANDBY";
        case SYSTEM_STATE_VOICE_ACTIVE:    return "VOICE_ACTIVE";
        case SYSTEM_STATE_TRANSITIONING:   return "TRANSITIONING";
        case SYSTEM_STATE_ERROR:           return "ERROR";
        case SYSTEM_STATE_SHUTDOWN:        return "SHUTDOWN";
        default:                           return "UNKNOWN";
    }
}

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
#include "esp_camera.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

static const char *TAG = TAG_STATE_MGR;
static system_state_t current_state = SYSTEM_STATE_INIT;
static system_state_t previous_state = SYSTEM_STATE_INIT;

// External globals from main.c
extern QueueHandle_t g_button_event_queue;
extern QueueHandle_t g_state_event_queue;
extern SemaphoreHandle_t g_i2s_config_mutex;
extern void websocket_connection_request_shutdown(void);

// Task handles for coordination
static TaskHandle_t camera_task_handle = NULL;
static TaskHandle_t stt_task_handle = NULL;
static TaskHandle_t tts_task_handle = NULL;

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

// ===========================
// Public Functions
// ===========================

void state_manager_task(void *pvParameters) {
    ESP_LOGI(TAG, "State manager task started on Core %d", xPortGetCoreID());
    ESP_LOGI(TAG, "Priority: %d", uxTaskPriorityGet(NULL));
    
    // Initialize camera mode first
    ESP_LOGI(TAG, "Starting in camera mode...");
    current_state = SYSTEM_STATE_TRANSITIONING;
    
    if (transition_to_camera_mode() == ESP_OK) {
        current_state = SYSTEM_STATE_CAMERA_STANDBY;
        ESP_LOGI(TAG, "✅ Entered CAMERA_STANDBY state");
        led_controller_set_state(LED_STATE_BREATHING);
    } else {
        ESP_LOGE(TAG, "❌ Failed to initialize camera - entering error state");
        current_state = SYSTEM_STATE_ERROR;
    }
    
    button_event_t button_event;
    uint32_t mode_switch_count = 0;
    esp_err_t ret;
    
    while (1) {
        // Reset watchdog timer
        esp_task_wdt_reset();
        
        // Wait for button events with timeout
        if (xQueueReceive(g_button_event_queue, &button_event, pdMS_TO_TICKS(100)) == pdTRUE) {
            ESP_LOGI(TAG, "Button event received: %d in state %s", 
                     button_event.type, state_to_string(current_state));
            
            // Only process events in stable states
            if (current_state != SYSTEM_STATE_TRANSITIONING) {
                switch (button_event.type) {
                    case BUTTON_EVENT_SINGLE_CLICK:
                        ESP_LOGI(TAG, "Single click - mode toggle requested");
                        mode_switch_count++;
                        
                        if (current_state == SYSTEM_STATE_CAMERA_STANDBY) {
                            ESP_LOGI(TAG, "Switching: Camera → Voice (count: %u)", (unsigned int)mode_switch_count);
                            previous_state = current_state;
                            current_state = SYSTEM_STATE_TRANSITIONING;
                            
                            if (transition_to_voice_mode() == ESP_OK) {
                                current_state = SYSTEM_STATE_VOICE_ACTIVE;
                                ESP_LOGI(TAG, "✅ Entered VOICE_ACTIVE state");
                            } else {
                                ESP_LOGE(TAG, "❌ Voice mode transition failed");
                                current_state = SYSTEM_STATE_ERROR;
                            }
                            
                        } else if (current_state == SYSTEM_STATE_VOICE_ACTIVE) {
                            ESP_LOGI(TAG, "Switching: Voice → Camera (count: %u)", (unsigned int)mode_switch_count);
                            previous_state = current_state;
                            current_state = SYSTEM_STATE_TRANSITIONING;
                            
                            if (transition_to_camera_mode() == ESP_OK) {
                                current_state = SYSTEM_STATE_CAMERA_STANDBY;
                                ESP_LOGI(TAG, "✅ Entered CAMERA_STANDBY state");
                            } else {
                                ESP_LOGE(TAG, "❌ Camera mode transition failed");
                                current_state = SYSTEM_STATE_ERROR;
                            }
                        }
                        break;
                        
                    case BUTTON_EVENT_DOUBLE_CLICK:
                        ESP_LOGI(TAG, "Double-click detected - triggering camera capture");

                        ret = handle_camera_capture();
                        if (ret != ESP_OK) {
                            ESP_LOGE(TAG, "Camera capture sequence failed");
                            led_controller_set_state(LED_STATE_SOS);
                            vTaskDelay(pdMS_TO_TICKS(2000));
                            led_state_t recovery_state =
                                (current_state == SYSTEM_STATE_VOICE_ACTIVE) ?
                                LED_STATE_SOLID : LED_STATE_BREATHING;
                            led_controller_set_state(recovery_state);
                        }
                        break;
                        
                    case BUTTON_EVENT_LONG_PRESS:
                        ESP_LOGW(TAG, "Long press - shutdown requested");
                        current_state = SYSTEM_STATE_SHUTDOWN;
                        break;
                        
                    default:
                        ESP_LOGW(TAG, "Unknown button event: %d", button_event.type);
                        break;
                }
            } else {
                ESP_LOGW(TAG, "Button event ignored - system transitioning");
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

esp_err_t state_manager_request_transition(system_state_t new_state) {
    ESP_LOGI(TAG, "State transition requested: %s -> %s", 
             state_to_string(current_state), state_to_string(new_state));
    
    // Validate transition
    if (current_state == SYSTEM_STATE_TRANSITIONING) {
        ESP_LOGW(TAG, "Cannot transition - already transitioning");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Post transition request to queue
    state_event_t event = {
        .type = STATE_EVENT_MODE_SWITCH_COMPLETE,
        .timestamp_ms = xTaskGetTickCount() * portTICK_PERIOD_MS
    };
    
    if (g_state_event_queue != NULL) {
        xQueueSend(g_state_event_queue, &event, pdMS_TO_TICKS(100));
    }
    
    return ESP_OK;
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
            return;  // Already complete, nothing to wait for
        }

        if ((xTaskGetTickCount() - guard_start) >= guard_timeout) {
            break;
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    const TickType_t overall_start = xTaskGetTickCount();
    const TickType_t overall_timeout = pdMS_TO_TICKS(VOICE_PIPELINE_STAGE_WAIT_MS);

    while (true) {
        websocket_pipeline_stage_t stage = websocket_client_get_pipeline_stage();
        bool pipeline_active = websocket_client_is_pipeline_active();

        if (!pipeline_active) {
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
                     (unsigned int)VOICE_PIPELINE_STAGE_WAIT_MS);
            break;
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (tts_decoder_has_pending_audio()) {
        size_t pending_bytes = tts_decoder_get_pending_bytes();
        ESP_LOGI(TAG, "Waiting for TTS playback drain (~%zu bytes pending, timeout %u ms)",
                 pending_bytes, (unsigned int)VOICE_TTS_FLUSH_WAIT_MS);

        esp_err_t wait_ret = tts_decoder_wait_for_idle(VOICE_TTS_FLUSH_WAIT_MS);
        if (wait_ret == ESP_OK) {
            ESP_LOGI(TAG, "TTS playback drained successfully");
        } else if (wait_ret == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "TTS playback drain timed out; proceeding with shutdown");
        } else if (wait_ret == ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "TTS decoder not initialized while waiting for drain");
        } else {
            ESP_LOGE(TAG, "Error while waiting for TTS drain: %s", esp_err_to_name(wait_ret));
        }
    }
}

static esp_err_t transition_to_camera_mode(void) {
    ESP_LOGI(TAG, "=== TRANSITION TO CAMERA MODE ===");
    
    esp_err_t ret = ESP_OK;
    
    // Step 1: Stop voice mode components if active
    if (previous_state == SYSTEM_STATE_VOICE_ACTIVE) {
        ESP_LOGI(TAG, "Stopping voice mode components...");
        
        // Stop STT pipeline and wait for downstream playback to drain
        stt_pipeline_stop();
        wait_for_voice_pipeline_shutdown();
        tts_decoder_stop();

        // Small delay for tasks to finish
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    // Step 2: Acquire I2S mutex (CRITICAL SECTION)
    ESP_LOGI(TAG, "Acquiring I2S mutex...");
    if (xSemaphoreTake(g_i2s_config_mutex, pdMS_TO_TICKS(STATE_TRANSITION_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire I2S mutex - timeout");
        return ESP_ERR_TIMEOUT;
    }
    
    // Step 3: Deinitialize audio drivers if active
    if (audio_driver_is_initialized()) {
        ESP_LOGI(TAG, "Deinitializing audio drivers...");
        ret = audio_driver_deinit();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to deinit audio: %s", esp_err_to_name(ret));
            xSemaphoreGive(g_i2s_config_mutex);
            return ret;
        }
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

    camera_fb_t *fb = camera_controller_capture_frame();
    if (fb == NULL) {
        ESP_LOGE(TAG, "Frame capture failed");
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

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Image uploaded successfully");
    } else {
        ESP_LOGE(TAG, "Image upload failed: %s", esp_err_to_name(ret));
    }

    return ret;
}

static esp_err_t handle_camera_capture(void) {
    ESP_LOGI(TAG, "Starting camera capture sequence");

    led_controller_set_state(LED_STATE_FLASH);
    esp_err_t capture_sound_ret = feedback_player_play(FEEDBACK_SOUND_CAPTURE);
    if (capture_sound_ret != ESP_OK) {
        ESP_LOGW(TAG, "Capture sound playback failed: %s", esp_err_to_name(capture_sound_ret));
    }

    bool capture_success = false;
    esp_err_t ret = ESP_OK;
    bool voice_pipeline_paused = false;
    bool audio_was_initialized = false;
    bool camera_initialized_here = false;

    // Fast path when camera is already armed
    if (current_state == SYSTEM_STATE_CAMERA_STANDBY) {
        if (!camera_controller_is_initialized()) {
            ESP_LOGW(TAG, "Camera not initialized in standby; attempting init");
            esp_err_t init_ret = camera_controller_init();
            if (init_ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to initialize camera: %s", esp_err_to_name(init_ret));
                return init_ret;
            }
        }
        ret = capture_and_upload_image();
        capture_success = (ret == ESP_OK);
        goto finalize_capture;
    }

    // If another state requested capture, try without touching audio path
    if (current_state != SYSTEM_STATE_VOICE_ACTIVE) {
        ESP_LOGW(TAG, "Camera capture requested during %s", state_to_string(current_state));
        ret = capture_and_upload_image();
        capture_success = (ret == ESP_OK);
        goto finalize_capture;
    }

    ESP_LOGI(TAG, "Pausing voice pipelines prior to capture");
    stt_pipeline_stop();
    wait_for_voice_pipeline_shutdown();
    tts_decoder_stop();
    voice_pipeline_paused = true;
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "Acquiring I2S mutex for capture");
    if (xSemaphoreTake(g_i2s_config_mutex, pdMS_TO_TICKS(STATE_TRANSITION_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire I2S mutex for capture");
        if (voice_pipeline_paused) {
            stt_pipeline_start();
            tts_decoder_start();
        }
        return ESP_ERR_TIMEOUT;
    }

    audio_was_initialized = audio_driver_is_initialized();

    if (audio_was_initialized) {
        ESP_LOGI(TAG, "Stopping audio driver before camera activation");
        ret = audio_driver_deinit();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Audio driver deinit failed: %s", esp_err_to_name(ret));
            goto restore_voice_path;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (!camera_controller_is_initialized()) {
        ESP_LOGI(TAG, "Initializing camera for capture");
        ret = camera_controller_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Camera init failed: %s", esp_err_to_name(ret));
            goto restore_voice_path;
        }
        camera_initialized_here = true;
    }

    ret = capture_and_upload_image();
    capture_success = (ret == ESP_OK);

restore_voice_path:
    if (camera_initialized_here) {
        camera_controller_deinit();
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (voice_pipeline_paused) {
        if (audio_was_initialized) {
            ESP_LOGI(TAG, "Restoring audio driver after capture");
            esp_err_t audio_ret = audio_driver_init();
            if (audio_ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to reinitialize audio driver: %s", esp_err_to_name(audio_ret));
                if (ret == ESP_OK) {
                    ret = audio_ret;
                }
            } else {
                stt_pipeline_start();
                tts_decoder_start();
            }
        } else {
            // Audio driver was already offline; just restart pipelines
            stt_pipeline_start();
            tts_decoder_start();
        }
    }

    xSemaphoreGive(g_i2s_config_mutex);
    goto finalize_capture;

finalize_capture:
    if (capture_success) {
        led_state_t next_led = (current_state == SYSTEM_STATE_VOICE_ACTIVE) ?
                               LED_STATE_SOLID : LED_STATE_BREATHING;
        led_controller_set_state(next_led);
    }

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
    ESP_LOGI(TAG, "  ✓ Mutex acquired (took %lld ms)", (long long)mutex_time);
    
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
        ESP_LOGE(TAG, "❌ Failed to deinit camera: %s (took %lld ms)", 
                 esp_err_to_name(ret), (long long)cam_deinit_time);
        xSemaphoreGive(g_i2s_config_mutex);
        return ret;
    }
    ESP_LOGI(TAG, "  ✓ Camera deinitialized (took %lld ms)", (long long)cam_deinit_time);
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
        ESP_LOGE(TAG, "❌ Failed to init audio: %s (took %lld ms)", 
                 esp_err_to_name(ret), (long long)audio_init_time);
        ESP_LOGE(TAG, "  Free heap at failure: %u bytes", (unsigned int)esp_get_free_heap_size());
        
        // Attempt recovery: reinitialize camera
        ESP_LOGW(TAG, "⚠ Attempting recovery - reinitializing camera");
        camera_controller_init();
        
        xSemaphoreGive(g_i2s_config_mutex);
        return ret;
    }
    ESP_LOGI(TAG, "  ✓ Audio initialized (took %lld ms)", (long long)audio_init_time);
    ESP_LOGI(TAG, "  Free heap after: %u bytes", (unsigned int)esp_get_free_heap_size());
    
    // Step 5: Release I2S mutex
    xSemaphoreGive(g_i2s_config_mutex);
    ESP_LOGI(TAG, "╔══════════════════════════════════════════════════");
    ESP_LOGI(TAG, "║ STEP 5: I2S mutex released");
    ESP_LOGI(TAG, "║ Total transition time: %lld ms", 
             (long long)(mutex_time + cam_deinit_time + 250 + audio_init_time));
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════════");
    
    // Step 6: Start STT and TTS pipelines
    ESP_LOGI(TAG, "╔══════════════════════════════════════════════════");
    ESP_LOGI(TAG, "║ STEP 6: Starting STT/TTS pipelines");
    ESP_LOGI(TAG, "╚══════════════════════════════════════════════════");

    ret = stt_pipeline_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start STT pipeline: %s", esp_err_to_name(ret));
    }
    
    ret = tts_decoder_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start TTS decoder: %s", esp_err_to_name(ret));
    }
    
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
    
    ESP_LOGI(TAG, "Requesting WebSocket shutdown");
    websocket_connection_request_shutdown();

    for (int i = 0; i < 40 && websocket_client_is_connected(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    
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
    if (transition_to_camera_mode() == ESP_OK) {
        current_state = SYSTEM_STATE_CAMERA_STANDBY;
        error_count = 0;  // Reset error count on success
        last_signaled_error = 0;
        ESP_LOGI(TAG, "✅ Recovery successful - back to camera mode");
    } else {
        current_state = SYSTEM_STATE_ERROR;
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

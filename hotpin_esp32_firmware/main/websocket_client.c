/**
 * @file websocket_client.c
 * @brief WebSocket client implementation for HotPin server communication
 * 
 * Handles:
 * - Connection to HotPin WebSocket server
 * - Session handshake protocol
 * - Binary PCM audio streaming (STT)
 * - Binary WAV audio reception (TTS)
 * - JSON status messages
 * - Automatic reconnection on disconnect
 */

#include "websocket_client.h"
#include "config.h"
#include "tts_decoder.h"
#include "led_controller.h"
#include "state_manager.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = TAG_WEBSOCKET;

// WebSocket client handle
static esp_websocket_client_handle_t g_ws_client = NULL;
static bool is_connected = false;
static bool is_initialized = false;
static bool is_started = false;
static char server_uri[128] = {0};
static volatile websocket_pipeline_stage_t g_pipeline_stage = WEBSOCKET_PIPELINE_STAGE_IDLE;

// Callback function pointer for incoming WAV audio data
static websocket_audio_callback_t g_audio_callback = NULL;
static void *g_audio_callback_arg = NULL;

// Callback function pointer for status messages
static websocket_status_callback_t g_status_callback = NULL;
static void *g_status_callback_arg = NULL;

// ===========================
// Private Function Declarations
// ===========================
static void websocket_event_handler(void *handler_args, esp_event_base_t base,
                                     int32_t event_id, void *event_data);
static void handle_text_message(const char *data, size_t len);
static void handle_binary_message(const uint8_t *data, size_t len);
static void update_pipeline_stage(const char *status, const char *stage);
static const char *pipeline_stage_to_string(websocket_pipeline_stage_t stage);

// ===========================
// Public Functions
// ===========================

esp_err_t websocket_client_init(const char *uri, const char *auth_token) {
    ESP_LOGI(TAG, "Initializing WebSocket client...");
    
    if (is_initialized) {
        ESP_LOGW(TAG, "WebSocket client already initialized");
        return ESP_OK;
    }
    
    if (uri == NULL) {
        ESP_LOGE(TAG, "Server URI is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    strncpy(server_uri, uri, sizeof(server_uri) - 1);
    ESP_LOGI(TAG, "Server URI: %s", server_uri);
    
    // Build authorization header if token provided
    static char headers[512] = {0};
    if (auth_token != NULL && strlen(auth_token) > 0) {
        snprintf(headers, sizeof(headers), 
                 "Authorization: Bearer %s\r\n", auth_token);
        ESP_LOGI(TAG, "Authorization header configured");
    }
    
    // Configure WebSocket client
    esp_websocket_client_config_t ws_cfg = {
        .uri = server_uri,
        .headers = (auth_token != NULL && strlen(auth_token) > 0) ? headers : NULL,
        .reconnect_timeout_ms = CONFIG_WEBSOCKET_RECONNECT_DELAY_MS,
        .network_timeout_ms = CONFIG_WEBSOCKET_TIMEOUT_MS,
        .buffer_size = 4096,  // Increase buffer for large audio chunks
        .task_stack = 6144,   // Sufficient stack for callbacks
        .task_prio = TASK_PRIORITY_WEBSOCKET,
        .disable_auto_reconnect = false,  // Enable auto-reconnect
    };
    
    g_ws_client = esp_websocket_client_init(&ws_cfg);
    if (g_ws_client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize WebSocket client");
        return ESP_FAIL;
    }
    
    // Register event handler
    esp_err_t ret = esp_websocket_register_events(g_ws_client, 
                                                    WEBSOCKET_EVENT_ANY,
                                                    websocket_event_handler, 
                                                    NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register event handler: %s", esp_err_to_name(ret));
        esp_websocket_client_destroy(g_ws_client);
        g_ws_client = NULL;
        return ret;
    }
    
    is_started = false;
    is_initialized = true;
    ESP_LOGI(TAG, "✅ WebSocket client initialized");
    
    return ESP_OK;
}

esp_err_t websocket_client_deinit(void) {
    ESP_LOGI(TAG, "Deinitializing WebSocket client...");
    
    if (!is_initialized) {
        ESP_LOGW(TAG, "WebSocket client not initialized");
        return ESP_OK;
    }
    
    // Disconnect if connected
    if (is_connected) {
        websocket_client_disconnect();
    }
    
    // Destroy client
    if (g_ws_client != NULL) {
        esp_err_t ret = esp_websocket_client_destroy(g_ws_client);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to destroy WebSocket client: %s", esp_err_to_name(ret));
            return ret;
        }
        g_ws_client = NULL;
    }
    
    is_connected = false;
    is_started = false;
    is_initialized = false;
    g_audio_callback = NULL;
    g_status_callback = NULL;
    
    ESP_LOGI(TAG, "WebSocket client deinitialized");
    return ESP_OK;
}

esp_err_t websocket_client_connect(void) {
    ESP_LOGI(TAG, "Connecting to WebSocket server...");
    
    if (!is_initialized || g_ws_client == NULL) {
        ESP_LOGE(TAG, "WebSocket client not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (is_connected) {
        ESP_LOGW(TAG, "Already connected");
        return ESP_OK;
    }
    
    esp_err_t ret = esp_websocket_client_start(g_ws_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket client: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "WebSocket client started");
    is_started = true;
    return ESP_OK;
}

esp_err_t websocket_client_disconnect(void) {
    ESP_LOGI(TAG, "Disconnecting from WebSocket server...");
    
    if (!is_initialized || g_ws_client == NULL) {
        ESP_LOGW(TAG, "WebSocket client not initialized");
        return ESP_OK;
    }
    
    if (!is_connected) {
        ESP_LOGW(TAG, "Not connected");
    } else {
        esp_err_t close_ret = esp_websocket_client_close(g_ws_client, portMAX_DELAY);
        if (close_ret != ESP_OK && close_ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Failed to close WebSocket: %s", esp_err_to_name(close_ret));
        }
    }

    esp_err_t ret = ESP_OK;
    if (is_started) {
        ret = esp_websocket_client_stop(g_ws_client);
        if (ret != ESP_OK) {
            if (ret == ESP_ERR_INVALID_STATE || ret == ESP_FAIL) {
                ESP_LOGW(TAG, "WebSocket client stop reported %s, continuing cleanup",
                         esp_err_to_name(ret));
            } else {
                ESP_LOGE(TAG, "Failed to stop WebSocket client: %s", esp_err_to_name(ret));
                return ret;
            }
        }
        is_started = false;
    }
    
    is_connected = false;
    g_pipeline_stage = WEBSOCKET_PIPELINE_STAGE_IDLE;
    ESP_LOGI(TAG, "WebSocket disconnected");
    
    return ESP_OK;
}

esp_err_t websocket_client_force_stop(void) {
    if (!is_initialized || g_ws_client == NULL) {
        is_connected = false;
        g_pipeline_stage = WEBSOCKET_PIPELINE_STAGE_IDLE;
        is_started = false;
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Force stopping WebSocket client");

    esp_err_t close_ret = esp_websocket_client_close(g_ws_client, 0);
    if (close_ret != ESP_OK && close_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Force close returned %s", esp_err_to_name(close_ret));
    }

    if (is_started) {
        esp_err_t stop_ret = esp_websocket_client_stop(g_ws_client);
        if (stop_ret == ESP_ERR_INVALID_STATE) {
            stop_ret = ESP_OK;
        } else if (stop_ret != ESP_OK) {
            ESP_LOGW(TAG, "Force stop returned %s", esp_err_to_name(stop_ret));
        }
        is_started = false;
    }

    is_connected = false;
    g_pipeline_stage = WEBSOCKET_PIPELINE_STAGE_IDLE;

    return ESP_OK;
}

esp_err_t websocket_client_send_handshake(void) {
    if (!is_connected) {
        ESP_LOGE(TAG, "Cannot send handshake - not connected");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Create handshake JSON
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object");
        return ESP_ERR_NO_MEM;
    }
    
    cJSON_AddStringToObject(root, "session_id", CONFIG_WEBSOCKET_SESSION_ID);
    
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    
    if (json_str == NULL) {
        ESP_LOGE(TAG, "Failed to serialize JSON");
        return ESP_ERR_NO_MEM;
    }
    
    ESP_LOGI(TAG, "Sending handshake: %s", json_str);
    
    int ret = esp_websocket_client_send_text(g_ws_client, json_str, strlen(json_str), portMAX_DELAY);
    free(json_str);
    
    if (ret < 0) {
        ESP_LOGE(TAG, "Failed to send handshake");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Handshake sent successfully");
    return ESP_OK;
}

esp_err_t websocket_client_send_audio(const uint8_t *data, size_t length) {
    if (!is_connected) {
        ESP_LOGE(TAG, "Cannot send audio - not connected");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (data == NULL || length == 0) {
        ESP_LOGE(TAG, "Invalid audio data");
        return ESP_ERR_INVALID_ARG;
    }
    
    int ret = esp_websocket_client_send_bin(g_ws_client, (const char *)data, length, portMAX_DELAY);
    if (ret < 0) {
        ESP_LOGE(TAG, "Failed to send audio chunk (%zu bytes)", length);
        return ESP_FAIL;
    }
    
    ESP_LOGD(TAG, "Sent %zu bytes of audio data", length);
    return ESP_OK;
}

esp_err_t websocket_client_send_text(const char *message) {
    if (!is_connected) {
        ESP_LOGE(TAG, "Cannot send text - not connected");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (message == NULL) {
        ESP_LOGE(TAG, "Message is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    int ret = esp_websocket_client_send_text(g_ws_client, message, strlen(message), portMAX_DELAY);
    if (ret < 0) {
        ESP_LOGE(TAG, "Failed to send text message");
        return ESP_FAIL;
    }
    
    ESP_LOGD(TAG, "Sent text message: %s", message);
    return ESP_OK;
}

esp_err_t websocket_client_send_eos(void) {
    const char *eos_msg = "{\"signal\":\"EOS\"}";
    ESP_LOGI(TAG, "Sending EOS signal");
    return websocket_client_send_text(eos_msg);
}

bool websocket_client_is_connected(void) {
    return is_connected;
}

void websocket_client_set_audio_callback(websocket_audio_callback_t callback, void *arg) {
    g_audio_callback = callback;
    g_audio_callback_arg = arg;
    ESP_LOGI(TAG, "Audio callback registered");
}

void websocket_client_set_status_callback(websocket_status_callback_t callback, void *arg) {
    g_status_callback = callback;
    g_status_callback_arg = arg;
    ESP_LOGI(TAG, "Status callback registered");
}

// ===========================
// Private Functions
// ===========================

static void websocket_event_handler(void *handler_args, esp_event_base_t base,
                                     int32_t event_id, void *event_data) {
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "✅ WebSocket connected to server");
            is_connected = true;
            g_pipeline_stage = WEBSOCKET_PIPELINE_STAGE_IDLE;
            is_started = true;
            
            // Send handshake immediately after connection
            websocket_client_send_handshake();
            
            if (g_status_callback) {
                g_status_callback(WEBSOCKET_STATUS_CONNECTED, g_status_callback_arg);
            }
            break;
            
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "⚠️ WebSocket disconnected");
            is_connected = false;
            g_pipeline_stage = WEBSOCKET_PIPELINE_STAGE_IDLE;
            is_started = false;
            
            if (g_status_callback) {
                g_status_callback(WEBSOCKET_STATUS_DISCONNECTED, g_status_callback_arg);
            }
            break;
            
        case WEBSOCKET_EVENT_DATA:
            ESP_LOGD(TAG, "Received data: opcode=%d, len=%d", data->op_code, data->data_len);
            
            if (data->op_code == 0x01) {  // Text frame
                handle_text_message((const char *)data->data_ptr, data->data_len);
            } else if (data->op_code == 0x02) {  // Binary frame
                handle_binary_message((const uint8_t *)data->data_ptr, data->data_len);
            }
            break;
            
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "❌ WebSocket error occurred");
            g_pipeline_stage = WEBSOCKET_PIPELINE_STAGE_IDLE;
            
            if (g_status_callback) {
                g_status_callback(WEBSOCKET_STATUS_ERROR, g_status_callback_arg);
            }
            break;
            
        default:
            ESP_LOGD(TAG, "Unhandled WebSocket event: %ld", event_id);
            break;
    }
}

static void handle_text_message(const char *data, size_t len) {
    // Parse JSON status messages
    char *json_str = strndup(data, len);
    if (json_str == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for JSON parsing");
        return;
    }
    
    ESP_LOGI(TAG, "Received text message: %s", json_str);
    
    cJSON *root = cJSON_Parse(json_str);
    free(json_str);
    
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        return;
    }
    
    // Check for status field
    cJSON *status = cJSON_GetObjectItem(root, "status");
    cJSON *stage = cJSON_GetObjectItem(root, "stage");
    const char *status_str = (status != NULL && cJSON_IsString(status)) ? status->valuestring : NULL;
    const char *stage_str = (stage != NULL && cJSON_IsString(stage)) ? stage->valuestring : NULL;

    if (status_str != NULL) {
        ESP_LOGI(TAG, "Server status: %s", status_str);
    }

    if (stage_str != NULL) {
        ESP_LOGI(TAG, "Server stage: %s", stage_str);
    }

    update_pipeline_stage(status_str, stage_str);
    
    // Check for transcription result
    cJSON *transcription = cJSON_GetObjectItem(root, "transcription");
    if (transcription != NULL && cJSON_IsString(transcription)) {
        ESP_LOGI(TAG, "Transcription: %s", transcription->valuestring);
    }
    
    cJSON_Delete(root);
}

static void handle_binary_message(const uint8_t *data, size_t len) {
    ESP_LOGI(TAG, "Received binary audio data: %zu bytes", len);
    
    // Call audio callback if registered
    if (g_audio_callback) {
        g_audio_callback(data, len, g_audio_callback_arg);
    } else {
        ESP_LOGW(TAG, "No audio callback registered - audio data discarded");
    }
}

static void update_pipeline_stage(const char *status, const char *stage) {
    if (status == NULL) {
        return;
    }

    websocket_pipeline_stage_t new_stage = g_pipeline_stage;
    bool notify_eos = false;
    bool voice_active = (state_manager_get_state() == SYSTEM_STATE_VOICE_ACTIVE);

    if (strcmp(status, "complete") == 0) {
        new_stage = WEBSOCKET_PIPELINE_STAGE_COMPLETE;
        notify_eos = true;
        if (voice_active) {
            led_controller_set_state(LED_STATE_SOLID);
        }
    } else if (strcmp(status, "processing") == 0) {
        if (stage != NULL) {
            if (strcmp(stage, "transcription") == 0) {
                new_stage = WEBSOCKET_PIPELINE_STAGE_TRANSCRIPTION;
                if (voice_active) {
                    led_controller_set_state(LED_STATE_PULSING);
                }
            } else if (strcmp(stage, "llm") == 0) {
                new_stage = WEBSOCKET_PIPELINE_STAGE_LLM;
                if (voice_active) {
                    led_controller_set_state(LED_STATE_PULSING);
                }
            } else if (strcmp(stage, "tts") == 0) {
                new_stage = WEBSOCKET_PIPELINE_STAGE_TTS;
                if (voice_active) {
                    led_controller_set_state(LED_STATE_SOLID);
                }
            }
        } else if (voice_active) {
            led_controller_set_state(LED_STATE_PULSING);
        }
    } else if (strcmp(status, "idle") == 0) {
        new_stage = WEBSOCKET_PIPELINE_STAGE_IDLE;
    }

    if (new_stage != g_pipeline_stage) {
        ESP_LOGI(TAG, "Pipeline stage changed: %s -> %s",
                 pipeline_stage_to_string(g_pipeline_stage),
                 pipeline_stage_to_string(new_stage));
        g_pipeline_stage = new_stage;
    }

    if (notify_eos) {
        tts_decoder_notify_end_of_stream();
    }
}

static const char *pipeline_stage_to_string(websocket_pipeline_stage_t stage) {
    switch (stage) {
        case WEBSOCKET_PIPELINE_STAGE_IDLE:
            return "idle";
        case WEBSOCKET_PIPELINE_STAGE_TRANSCRIPTION:
            return "transcription";
        case WEBSOCKET_PIPELINE_STAGE_LLM:
            return "llm";
        case WEBSOCKET_PIPELINE_STAGE_TTS:
            return "tts";
        case WEBSOCKET_PIPELINE_STAGE_COMPLETE:
            return "complete";
        default:
            return "unknown";
    }
}

websocket_pipeline_stage_t websocket_client_get_pipeline_stage(void) {
    return g_pipeline_stage;
}

bool websocket_client_is_pipeline_active(void) {
    websocket_pipeline_stage_t stage = g_pipeline_stage;
    return stage == WEBSOCKET_PIPELINE_STAGE_TRANSCRIPTION ||
           stage == WEBSOCKET_PIPELINE_STAGE_LLM ||
           stage == WEBSOCKET_PIPELINE_STAGE_TTS;
}

const char *websocket_client_pipeline_stage_to_string(websocket_pipeline_stage_t stage) {
    return pipeline_stage_to_string(stage);
}

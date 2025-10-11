/**
 * @file websocket_client_stub.c
 * @brief Temporary WebSocket client stub implementation
 * 
 * This is a placeholder implementation until the WebSocket component issue is resolved.
 * All functions return success but don't perform actual WebSocket operations.
 */

#include "websocket_client.h"
#include "config.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = TAG_WEBSOCKET;

// Stub global variables
static bool is_connected = false;
static bool is_initialized = false;
static char server_uri[128] = {0};
static websocket_pipeline_stage_t g_pipeline_stage = WEBSOCKET_PIPELINE_STAGE_IDLE;

// Callback function pointers (stubs)
static websocket_audio_callback_t g_audio_callback = NULL;
static void *g_audio_callback_arg = NULL;
static websocket_status_callback_t g_status_callback = NULL;
static void *g_status_callback_arg = NULL;

// ===========================
// Public Functions (Stub Implementation)
// ===========================

esp_err_t websocket_client_init(const char *uri, const char *auth_token) {
    ESP_LOGW(TAG, "WebSocket client STUB initialized (no actual connection)");
    
    if (uri != NULL) {
        strncpy(server_uri, uri, sizeof(server_uri) - 1);
        ESP_LOGI(TAG, "Would connect to: %s", server_uri);
    }
    
    if (auth_token != NULL) {
        ESP_LOGI(TAG, "Would use Bearer token: %.*s...", 8, auth_token);
    }
    
    is_initialized = true;
    return ESP_OK;
}

esp_err_t websocket_client_deinit(void) {
    ESP_LOGW(TAG, "WebSocket client STUB deinitialized");
    is_initialized = false;
    is_connected = false;
    return ESP_OK;
}

esp_err_t websocket_client_connect(void) {
    ESP_LOGW(TAG, "WebSocket client STUB connect (no actual connection)");
    if (!is_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    is_connected = true;
    g_pipeline_stage = WEBSOCKET_PIPELINE_STAGE_IDLE;
    
    // Simulate connected status callback
    if (g_status_callback) {
        g_status_callback(WEBSOCKET_STATUS_CONNECTED, g_status_callback_arg);
    }
    
    return ESP_OK;
}

esp_err_t websocket_client_disconnect(void) {
    ESP_LOGW(TAG, "WebSocket client STUB disconnect");
    is_connected = false;
    g_pipeline_stage = WEBSOCKET_PIPELINE_STAGE_IDLE;
    
    // Simulate disconnected status callback
    if (g_status_callback) {
        g_status_callback(WEBSOCKET_STATUS_DISCONNECTED, g_status_callback_arg);
    }
    
    return ESP_OK;
}

esp_err_t websocket_client_send_audio(const uint8_t *data, size_t length) {
    ESP_LOGD(TAG, "WebSocket client STUB: would send %d bytes audio data", length);
    (void)data; // Suppress unused parameter warning
    return ESP_OK;
}

esp_err_t websocket_client_send_text(const char *message) {
    ESP_LOGD(TAG, "WebSocket client STUB: would send text: %s", message);
    return ESP_OK;
}

esp_err_t websocket_client_send_handshake(void) {
    ESP_LOGI(TAG, "WebSocket client STUB: would send handshake");
    
    // Simulate handshake JSON creation
    cJSON *root = cJSON_CreateObject();
    if (root != NULL) {
        cJSON_AddStringToObject(root, "session_id", CONFIG_WEBSOCKET_SESSION_ID);
        char *json_str = cJSON_PrintUnformatted(root);
        if (json_str != NULL) {
            ESP_LOGI(TAG, "Would send handshake: %s", json_str);
            free(json_str);
        }
        cJSON_Delete(root);
    }
    
    return ESP_OK;
}

esp_err_t websocket_client_send_eos(void) {
    ESP_LOGI(TAG, "WebSocket client STUB: would send EOS signal");
    return ESP_OK;
}

bool websocket_client_is_connected(void) {
    return is_connected;
}

void websocket_client_set_audio_callback(websocket_audio_callback_t callback, void *arg) {
    g_audio_callback = callback;
    g_audio_callback_arg = arg;
    ESP_LOGI(TAG, "WebSocket client STUB: audio callback registered");
}

void websocket_client_set_status_callback(websocket_status_callback_t callback, void *arg) {
    g_status_callback = callback;
    g_status_callback_arg = arg;
    ESP_LOGI(TAG, "WebSocket client STUB: status callback registered");
}

websocket_pipeline_stage_t websocket_client_get_pipeline_stage(void) {
    return g_pipeline_stage;
}

bool websocket_client_is_pipeline_active(void) {
    return false;
}

const char *websocket_client_pipeline_stage_to_string(websocket_pipeline_stage_t stage) {
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
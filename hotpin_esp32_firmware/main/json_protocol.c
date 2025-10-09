/**
 * @file json_protocol.c
 * @brief JSON protocol implementation
 */

#include "json_protocol.h"
#include "config.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "JSON_PROTO";

int json_protocol_build_start(const char *session_id, char *buffer, size_t buffer_size) {
    if (session_id == NULL || buffer == NULL || buffer_size == 0) {
        ESP_LOGE(TAG, "Invalid arguments");
        return -1;
    }
    
    int written = snprintf(buffer, buffer_size,
                           "{\"type\":\"start\",\"session\":\"%s\",\"sampleRate\":16000,\"channels\":1}",
                           session_id);
    
    if (written < 0 || (size_t)written >= buffer_size) {
        ESP_LOGE(TAG, "Buffer too small for start message");
        return -1;
    }
    
    ESP_LOGD(TAG, "Built start message: %s", buffer);
    return written;
}

int json_protocol_build_end(const char *session_id, char *buffer, size_t buffer_size) {
    if (session_id == NULL || buffer == NULL || buffer_size == 0) {
        ESP_LOGE(TAG, "Invalid arguments");
        return -1;
    }
    
    int written = snprintf(buffer, buffer_size,
                           "{\"type\":\"end\",\"session\":\"%s\"}",
                           session_id);
    
    if (written < 0 || (size_t)written >= buffer_size) {
        ESP_LOGE(TAG, "Buffer too small for end message");
        return -1;
    }
    
    ESP_LOGD(TAG, "Built end message: %s", buffer);
    return written;
}

int json_protocol_generate_session_id(char *buffer, size_t buffer_size) {
    if (buffer == NULL || buffer_size == 0) {
        ESP_LOGE(TAG, "Invalid arguments");
        return -1;
    }
    
    // Get MAC address for unique device ID
    uint8_t mac[6];
    esp_err_t ret = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read MAC address, using default");
        memset(mac, 0, sizeof(mac));
    }
    
    // Get current timestamp
    int64_t timestamp = esp_timer_get_time() / 1000000;  // Convert to seconds
    
    // Format: hotpin-AABBCC-timestamp
    int written = snprintf(buffer, buffer_size,
                           "hotpin-%02X%02X%02X-%lld",
                           mac[3], mac[4], mac[5], timestamp);
    
    if (written < 0 || (size_t)written >= buffer_size) {
        ESP_LOGE(TAG, "Buffer too small for session ID");
        return -1;
    }
    
    ESP_LOGI(TAG, "Generated session ID: %s", buffer);
    return written;
}

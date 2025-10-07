/**
 * @file http_client.c
 * @brief HTTP client implementation for image upload
 */

#include "http_client.h"
#include "config.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

// ===========================
// Constants
// ===========================
#define BOUNDARY_STRING "----HotPinESP32CamBoundary"
#define MAX_HTTP_RECV_BUFFER 1024

// ===========================
// Private Variables
// ===========================
static const char *TAG = "HTTP_CLIENT";
static char server_url[128] = {0};
static char auth_token[256] = {0};
static bool is_initialized = false;

// ===========================
// Private Functions
// ===========================

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER: %s: %s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            if (evt->user_data != NULL && evt->data_len > 0) {
                // Copy response data to user buffer
                char *response_buf = (char *)evt->user_data;
                int remaining_space = MAX_HTTP_RECV_BUFFER - strlen(response_buf) - 1;
                int copy_len = (evt->data_len < remaining_space) ? evt->data_len : remaining_space;
                strncat(response_buf, (char *)evt->data, copy_len);
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
        default:
            break;
    }
    return ESP_OK;
}

// ===========================
// Public Functions
// ===========================

esp_err_t http_client_init(const char *server_url_param, const char *auth_token_param) {
    ESP_LOGI(TAG, "Initializing HTTP client");
    
    if (server_url_param == NULL) {
        ESP_LOGE(TAG, "Server URL is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    strncpy(server_url, server_url_param, sizeof(server_url) - 1);
    
    if (auth_token_param != NULL) {
        strncpy(auth_token, auth_token_param, sizeof(auth_token) - 1);
        ESP_LOGI(TAG, "Authorization token configured");
    } else {
        auth_token[0] = '\0';
        ESP_LOGW(TAG, "No authorization token provided");
    }
    
    is_initialized = true;
    ESP_LOGI(TAG, "HTTP client initialized (server: %s)", server_url);
    return ESP_OK;
}

esp_err_t http_client_deinit(void) {
    ESP_LOGI(TAG, "Deinitializing HTTP client");
    is_initialized = false;
    return ESP_OK;
}

esp_err_t http_client_upload_image(const char *session_id,
                                     const uint8_t *jpeg_data,
                                     size_t jpeg_len,
                                     char *response_buffer,
                                     size_t response_buffer_size) {
    if (!is_initialized) {
        ESP_LOGE(TAG, "HTTP client not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (session_id == NULL || jpeg_data == NULL || jpeg_len == 0) {
        ESP_LOGE(TAG, "Invalid arguments");
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Uploading image: session=%s, size=%zu bytes", session_id, jpeg_len);
    
    // Construct full URL
    char url[256];
    snprintf(url, sizeof(url), "%s%s", server_url, CONFIG_HTTP_IMAGE_ENDPOINT);
    
    // Build multipart/form-data body
    // Part 1: session field
    char session_part[512];
    snprintf(session_part, sizeof(session_part),
             "--%s\r\n"
             "Content-Disposition: form-data; name=\"session\"\r\n\r\n"
             "%s\r\n",
             BOUNDARY_STRING, session_id);
    
    // Part 2: file field header
    char file_header[256];
    snprintf(file_header, sizeof(file_header),
             "--%s\r\n"
             "Content-Disposition: form-data; name=\"file\"; filename=\"image.jpg\"\r\n"
             "Content-Type: image/jpeg\r\n\r\n",
             BOUNDARY_STRING);
    
    // Part 3: closing boundary
    char closing_boundary[64];
    snprintf(closing_boundary, sizeof(closing_boundary),
             "\r\n--%s--\r\n",
             BOUNDARY_STRING);
    
    // Calculate total length
    size_t total_len = strlen(session_part) + strlen(file_header) + 
                       jpeg_len + strlen(closing_boundary);
    
    // Allocate temporary buffer for entire POST body
    char *post_data = heap_caps_malloc(total_len, MALLOC_CAP_SPIRAM);
    if (post_data == NULL) {
        ESP_LOGE(TAG, "Failed to allocate %zu bytes for POST data", total_len);
        return ESP_ERR_NO_MEM;
    }
    
    // Assemble POST body
    size_t offset = 0;
    memcpy(post_data + offset, session_part, strlen(session_part));
    offset += strlen(session_part);
    memcpy(post_data + offset, file_header, strlen(file_header));
    offset += strlen(file_header);
    memcpy(post_data + offset, jpeg_data, jpeg_len);
    offset += jpeg_len;
    memcpy(post_data + offset, closing_boundary, strlen(closing_boundary));
    
    ESP_LOGI(TAG, "POST body assembled: %zu bytes", total_len);
    
    // Configure HTTP client
    char content_type_header[128];
    snprintf(content_type_header, sizeof(content_type_header),
             "multipart/form-data; boundary=%s", BOUNDARY_STRING);
    
    // Prepare response buffer
    char local_response[MAX_HTTP_RECV_BUFFER] = {0};
    char *response_target = (response_buffer != NULL) ? response_buffer : local_response;
    if (response_buffer != NULL && response_buffer_size > 0) {
        response_buffer[0] = '\0';
    }
    
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = CONFIG_HTTP_TIMEOUT_MS,
        .event_handler = http_event_handler,
        .user_data = response_target,
        .buffer_size = 4096,
        .buffer_size_tx = 4096
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        free(post_data);
        return ESP_FAIL;
    }
    
    // Set headers
    esp_http_client_set_header(client, "Content-Type", content_type_header);
    
    if (auth_token[0] != '\0') {
        char auth_header[300];
        snprintf(auth_header, sizeof(auth_header), "Bearer %s", auth_token);
        esp_http_client_set_header(client, "Authorization", auth_header);
        ESP_LOGD(TAG, "Authorization header set");
    }
    
    esp_http_client_set_post_field(client, post_data, total_len);
    
    // Perform HTTP request
    ESP_LOGI(TAG, "Sending POST request to %s", url);
    esp_err_t err = esp_http_client_perform(client);
    
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        int content_length = esp_http_client_get_content_length(client);
        
        ESP_LOGI(TAG, "HTTP POST Status = %d, content_length = %d", status_code, content_length);
        
        if (status_code >= 200 && status_code < 300) {
            ESP_LOGI(TAG, "Image uploaded successfully");
            if (response_target[0] != '\0') {
                ESP_LOGI(TAG, "Server response: %s", response_target);
            }
        } else {
            ESP_LOGW(TAG, "Server returned non-2xx status: %d", status_code);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "HTTP POST failed: %s", esp_err_to_name(err));
    }
    
    // Cleanup
    esp_http_client_cleanup(client);
    free(post_data);
    
    return err;
}

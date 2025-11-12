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
#include "event_dispatcher.h"
#include "system_events.h"
#include "stt_pipeline.h"
#include "tts_decoder.h"
#include "audio_feedback.h"
#include "feedback_player.h"  // ✅ Added for processing/completion feedback sounds
#include "led_controller.h"   // ✅ Added for LED state feedback during processing
#include "state_manager.h"    // ✅ Added for state checking before TTS start
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_task_wdt.h"  // Added for esp_task_wdt_reset
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "inttypes.h"
#include <string.h>

// Safe watchdog reset function to prevent errors when task is not registered
static const char *TAG = TAG_WEBSOCKET;
static TaskHandle_t s_health_check_task_handle = NULL;

// Safe watchdog reset function to prevent errors when task is not registered
// ✅ IMPROVED: Suppress common benign errors (ESP_ERR_NOT_FOUND, ESP_ERR_INVALID_ARG)
// These occur during task shutdown and are not actual errors
extern TaskHandle_t g_websocket_task_handle;

// Only tasks that are registered with the TWDT should attempt a reset; otherwise
// ESP-IDF logs an error even though we suppress it locally. Guard by task handle.
static inline void safe_task_wdt_reset(void) {
    TaskHandle_t current = xTaskGetCurrentTaskHandle();
    if (current == NULL) {
        return;
    }

    if (current != g_websocket_task_handle && current != s_health_check_task_handle) {
        return;  // Skip resets for unregistered helper tasks (reconnect, event loop, etc.)
    }

    esp_err_t ret = esp_task_wdt_reset();
    if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND && ret != ESP_ERR_INVALID_ARG) {
        ESP_LOGD(TAG, "WDT reset failed: %s", esp_err_to_name(ret));
    }
}

// WebSocket client handle
static esp_websocket_client_handle_t g_ws_client = NULL;
static bool is_connected = false;
static bool is_initialized = false;
static bool is_started = false;
static char server_uri[128] = {0};
static volatile websocket_pipeline_stage_t g_pipeline_stage = WEBSOCKET_PIPELINE_STAGE_IDLE;
static volatile bool g_session_ready = false;
static uint32_t s_reconnect_attempt_count = 0;
static uint32_t s_last_reconnect_delay = CONFIG_WEBSOCKET_RECONNECT_DELAY_MS;

// Health check tracking variables
// (Removed unused tracking variables to eliminate compiler warnings)

// Callback function pointer for incoming WAV audio data
static websocket_audio_callback_t g_audio_callback = NULL;
static void *g_audio_callback_arg = NULL;

// Callback function pointer for status messages
static websocket_status_callback_t g_status_callback = NULL;
static void *g_status_callback_arg = NULL;

// Task handles for reconnect tasks
static TaskHandle_t s_reconnect_task_handle = NULL;
static TaskHandle_t s_delayed_reconnect_task_handle = NULL;

// ===========================
// Private Function Declarations
// ===========================
static void websocket_event_handler(void *handler_args, esp_event_base_t base,
                                     int32_t event_id, void *event_data);
static void handle_text_message(const char *data, size_t len);
static void handle_binary_message(const uint8_t *data, size_t len);
static void update_pipeline_stage(const char *status, const char *stage);
static const char *pipeline_stage_to_string(websocket_pipeline_stage_t stage);
static void post_pipeline_stage_event(websocket_pipeline_stage_t stage);
// Enhanced connection health check task
static void websocket_health_check_task(void *pvParameters);
// Helper functions for WebSocket reconnection tasks
static void websocket_reconnect_task(void *pvParameters);
static void websocket_delayed_reconnect_task(void *pvParameters);

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
    
    // Configure WebSocket client with enhanced reliability settings
    // Using correct field names for ESP-IDF 5.4.2
    esp_websocket_client_config_t ws_cfg = {
        .uri = server_uri,
        .headers = (auth_token != NULL && strlen(auth_token) > 0) ? headers : NULL,
        .reconnect_timeout_ms = CONFIG_WEBSOCKET_RECONNECT_DELAY_MS,
        .network_timeout_ms = CONFIG_WEBSOCKET_TIMEOUT_MS,
        .buffer_size = 65536,  // 🔧 INCREASED: 64KB buffer to prevent overflow during burst transmission (was 32KB)
        .task_stack = 8192,   // Increased stack for callbacks
        .task_prio = TASK_PRIORITY_WEBSOCKET,
        .disable_auto_reconnect = true,   // Use manual reconnection logic to avoid double-start races
        .keep_alive_enable = true,         // Enable TCP keepalive
        .keep_alive_idle = 10,             // 🔧 TUNED: More aggressive keepalive - 10s idle (was 45s) prevents router/FW closure
        .keep_alive_interval = 5,          // 🔧 TUNED: Faster keepalive probes - 5s interval (was 15s) detects failures quickly
        .ping_interval_sec = 10,           // CRITICAL: Send WebSocket ping every 10s to keep connection alive
        .cert_pem = NULL,                 // No certificate validation for now
        .transport = WEBSOCKET_TRANSPORT_OVER_TCP, // Explicit TCP transport
        .use_global_ca_store = false,      // No global CA store
        .skip_cert_common_name_check = true, // Skip certificate checks
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
    
    // Force stop the client to ensure proper cleanup
    websocket_client_force_stop();
    
    // Destroy client
    if (g_ws_client != NULL) {
        esp_err_t ret = esp_websocket_client_destroy(g_ws_client);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to destroy WebSocket client: %s", esp_err_to_name(ret));
            // Don't return error here as we want to continue cleanup
        }
        g_ws_client = NULL;  // Set to NULL to prevent double-free
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
    
    // Sync with driver state in case internal flags drifted
    if (esp_websocket_client_is_connected(g_ws_client)) {
        if (!is_connected) {
            ESP_LOGW(TAG, "WebSocket client already connected (syncing internal flags)");
        } else {
            ESP_LOGD(TAG, "WebSocket client already connected");
        }
        is_connected = true;
        is_started = true;
        return ESP_OK;
    }

    if (is_connected) {
        ESP_LOGW(TAG, "Already connected");
        return ESP_OK;
    }
    
    // Stop any existing connection attempt first to clean up state
    if (is_started) {
        if (esp_websocket_client_is_connected(g_ws_client)) {
            ESP_LOGD(TAG, "WebSocket client start already in progress and link is healthy");
            return ESP_OK;
        }

        ESP_LOGW(TAG, "WebSocket client marked as started but link not connected - forcing restart");
        esp_err_t stop_ret = esp_websocket_client_stop(g_ws_client);
        if (stop_ret != ESP_OK && stop_ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "WebSocket client stop before connect: %s", esp_err_to_name(stop_ret));
        }
        is_started = false;
    }
    
    // CRITICAL FIX: Enhanced reconnection strategy with better backoff and health checks
    if (s_reconnect_attempt_count > 0) {
        // Calculate next delay: base delay * 2^(attempt count), with max and jitter
        uint32_t next_delay = s_last_reconnect_delay;
        if (next_delay < 60000) { // Max 60 seconds (increased from 30)
            next_delay = (s_last_reconnect_delay * 2 < 60000) ? s_last_reconnect_delay * 2 : 60000;
        }
        
        // Add jitter to prevent thundering herd effect
        uint32_t jitter = esp_random() % (next_delay / 3); // Add up to 33% jitter (increased from 25%)
        uint32_t final_delay = next_delay + jitter;
        
        ESP_LOGI(TAG, "Reconnect attempt %u, waiting %u ms (with jitter)", 
                 (unsigned int)s_reconnect_attempt_count, (unsigned int)final_delay);
        
        // CRITICAL FIX: Reset watchdog during long delays to prevent system reset
        uint32_t delay_elapsed = 0;
        const uint32_t delay_step = 1000; // 1 second steps
        while (delay_elapsed < final_delay) {
            vTaskDelay(pdMS_TO_TICKS(delay_step < (final_delay - delay_elapsed) ? delay_step : (final_delay - delay_elapsed)));
            delay_elapsed += delay_step;
            safe_task_wdt_reset(); // Reset watchdog during long delays
        }
        
        s_last_reconnect_delay = next_delay;
    }
    
    // CRITICAL FIX: Add network connectivity check before attempting connection
    // TODO: Add actual network connectivity check here if needed
    esp_err_t network_check = ESP_OK; // Declare and use variable to prevent unused warning
    (void)network_check; // Explicitly mark as unused to prevent compiler warning
    
    esp_err_t ret = esp_websocket_client_start(g_ws_client);
    if (ret != ESP_OK) {
        // Treat duplicate start attempts as success if the transport is already running
        if ((ret == ESP_ERR_INVALID_STATE || ret == ESP_FAIL) &&
            esp_websocket_client_is_connected(g_ws_client)) {
            ESP_LOGW(TAG, "WebSocket client already active, ignoring duplicate start request");
            is_started = true;
            is_connected = true;
            s_reconnect_attempt_count = 0;
            s_last_reconnect_delay = CONFIG_WEBSOCKET_RECONNECT_DELAY_MS;
            return ESP_OK;
        }

        s_reconnect_attempt_count++; // Increment attempt counter on failure
        ESP_LOGE(TAG, "Failed to start WebSocket client: %s (attempt %u)", 
                 esp_err_to_name(ret), (unsigned int)s_reconnect_attempt_count);
        
        // CRITICAL FIX: If too many failures, force a more aggressive cleanup
        if (s_reconnect_attempt_count > 5) {
            ESP_LOGW(TAG, "Too many connection failures (%u), forcing client recreation", 
                     (unsigned int)s_reconnect_attempt_count);
            websocket_client_force_stop();
            // Reset counters to allow clean restart
            s_reconnect_attempt_count = 0;
            s_last_reconnect_delay = CONFIG_WEBSOCKET_RECONNECT_DELAY_MS;
        }
        
        return ret;
    }
    
    s_reconnect_attempt_count = 0; // Reset counter on successful connection
    s_last_reconnect_delay = CONFIG_WEBSOCKET_RECONNECT_DELAY_MS; // Reset delay as well
    
    // ✅ FIX: DISABLED redundant health check task
    // Connection management is now centralized in websocket_connection_task in main.c
    // This eliminates watchdog conflicts and simplifies error recovery
    // The health check task was competing with state_manager and causing deadlocks
    /*
    BaseType_t health_ret = xTaskCreate(
        websocket_health_check_task,
        "ws_health_check",
        2048,
        NULL,
        TASK_PRIORITY_WEBSOCKET - 2,
        &s_health_check_task_handle
    );
    
    if (health_ret != pdPASS) {
        ESP_LOGW(TAG, "Failed to create WebSocket health check task: %d", (int)health_ret);
        s_health_check_task_handle = NULL;
    } else {
        ESP_LOGI(TAG, "✅ WebSocket health check task created");
        esp_err_t wdt_ret = esp_task_wdt_add(s_health_check_task_handle);
        if (wdt_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to add health check task to WDT: %s", esp_err_to_name(wdt_ret));
        } else {
            ESP_LOGD(TAG, "Health check task added to WDT successfully");
        }
    }
    */
    s_health_check_task_handle = NULL; // No health check task
    
    ESP_LOGI(TAG, "WebSocket client started (health check disabled - managed by main connection task)");
    is_started = true;
    return ESP_OK;
}

// Enhanced connection health check with better error handling
static void websocket_health_check_task(void *pvParameters) {
    ESP_LOGI(TAG, "WebSocket health check task started");
    
    // Track connection state for better diagnostics
    static bool s_last_connected = false;
    static uint32_t s_disconnect_count = 0;
    static uint32_t s_reconnect_success_count = 0;
    static uint32_t s_health_check_cycle_count = 0;
    
    const TickType_t health_check_period = pdMS_TO_TICKS(5000); // 5 second intervals
    const TickType_t reconnect_cooldown = pdMS_TO_TICKS(30000); // 30 second cooldown after reconnect
    
    TickType_t last_reconnect_attempt = 0;
    
    while (1) {
        // Wait for health check period or shutdown signal
        uint32_t notification = ulTaskNotifyTake(pdTRUE, health_check_period);
        if (notification > 0) {
            // Shutdown signal received
            ESP_LOGI(TAG, "WebSocket health check task shutdown signal received");
            break;
        }
        
        s_health_check_cycle_count++;
        
        // Check connection status
        bool currently_connected = websocket_client_is_connected();
        
        // Log connection state changes
        if (currently_connected != s_last_connected) {
            if (currently_connected) {
                ESP_LOGI(TAG, "WebSocket connection restored (cycle: %u, disconnects: %u, reconnects: %u)",
                         (unsigned int)s_health_check_cycle_count,
                         (unsigned int)s_disconnect_count,
                         (unsigned int)s_reconnect_success_count);
                s_reconnect_success_count++;
            } else {
                ESP_LOGW(TAG, "WebSocket connection lost (cycle: %u, disconnects: %u, reconnects: %u)",
                         (unsigned int)s_health_check_cycle_count,
                         (unsigned int)s_disconnect_count,
                         (unsigned int)s_reconnect_success_count);
                s_disconnect_count++;
            }
            s_last_connected = currently_connected;
        }
        
        // If disconnected, attempt reconnection with backoff
        if (!currently_connected && is_initialized) {
            TickType_t now = xTaskGetTickCount();
            
            // Apply cooldown after last reconnect attempt
            if ((now - last_reconnect_attempt) >= reconnect_cooldown) {
                ESP_LOGI(TAG, "Attempting WebSocket reconnection (disconnects: %u, cycle: %u)",
                         (unsigned int)s_disconnect_count, (unsigned int)s_health_check_cycle_count);
                
                // Before attempting reconnection, ensure the client is in a clean state
                websocket_client_force_stop();
                
                last_reconnect_attempt = now;
                
                // Attempt reconnection
                esp_err_t ret = websocket_client_connect();
                if (ret == ESP_OK) {
                    ESP_LOGI(TAG, "WebSocket reconnection successful");
                    s_reconnect_success_count++;
                } else {
                    ESP_LOGW(TAG, "WebSocket reconnection failed: %s", esp_err_to_name(ret));
                    // Use exponential backoff for failed reconnection attempts
                    // Start with 5s, then 10s, 20s, max 60s
                    uint32_t fail_delay = 5000 * (1 << (s_disconnect_count % 4)); // Max ~40s
                    if (fail_delay > 60000) fail_delay = 60000; // Cap at 60s
                    
                    ESP_LOGD(TAG, "Reconnection fail delay: %u ms", (unsigned int)fail_delay);
                    
                    // Wait with periodic WDT resets to avoid system reset during long delays
                    uint32_t elapsed = 0;
                    const uint32_t step = 1000; // 1 second steps
                    while (elapsed < fail_delay && !ulTaskNotifyTake(pdTRUE, 0)) { // Check for shutdown signal
                        vTaskDelay(pdMS_TO_TICKS(step < (fail_delay - elapsed) ? step : (fail_delay - elapsed)));
                        elapsed += step;
                        safe_task_wdt_reset();
                    }
                    
                    if (elapsed >= fail_delay) {
                        ESP_LOGD(TAG, "Reconnection delay completed, will retry on next cycle");
                    } else {
                        ESP_LOGI(TAG, "Shutdown signal received during reconnection delay");
                        break; // Exit if shutdown signal received during delay
                    }
                }
            } else {
                // Still in cooldown period
                TickType_t remaining_cooldown = reconnect_cooldown - (now - last_reconnect_attempt);
                if (remaining_cooldown > 0) {
                    ESP_LOGD(TAG, "WebSocket in reconnect cooldown (%u ms remaining)",
                             (unsigned int)(remaining_cooldown * portTICK_PERIOD_MS));
                }
            }
        }
        
        // Reset watchdog to prevent system reset during health checks
        safe_task_wdt_reset();
    }
    
    ESP_LOGI(TAG, "WebSocket health check task exiting (cycles: %u, disconnects: %u, reconnects: %u)",
             (unsigned int)s_health_check_cycle_count,
             (unsigned int)s_disconnect_count,
             (unsigned int)s_reconnect_success_count);
    
    // Remove this task from WDT before deleting it
    esp_err_t wdt_ret = esp_task_wdt_delete(NULL);
    if (wdt_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to remove health check task from WDT: %s", esp_err_to_name(wdt_ret));
    }
    
    // Reset the global handle since this task is being deleted
    s_health_check_task_handle = NULL;
    
    vTaskDelete(NULL);
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
    g_session_ready = false;
    s_reconnect_attempt_count = 0; // Reset reconnect counter on explicit disconnect
    
    // Clean up the health check task if it exists
    if (s_health_check_task_handle != NULL) {
        // Notify the health check task to exit by sending a notification
        xTaskNotify(s_health_check_task_handle, 1, eSetValueWithOverwrite);
        
        // Wait a bit for the task to terminate gracefully
        vTaskDelay(pdMS_TO_TICKS(100));
        
        // Remove from WDT if still registered
        if (s_health_check_task_handle != NULL) {
            esp_task_wdt_delete(s_health_check_task_handle);
        }
        
        // Reset the handle
        s_health_check_task_handle = NULL;
    }
    
    ESP_LOGI(TAG, "WebSocket disconnected");
    
    return ESP_OK;
}

esp_err_t websocket_client_force_stop(void) {
    if (!is_initialized || g_ws_client == NULL) {
        is_connected = false;
        g_pipeline_stage = WEBSOCKET_PIPELINE_STAGE_IDLE;
        g_session_ready = false;
        is_started = false;
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Force stopping WebSocket client");

    // First try to close the connection gracefully
    esp_err_t close_ret = esp_websocket_client_close(g_ws_client, 1000); // 1 second timeout for close
    if (close_ret != ESP_OK && close_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Graceful close returned %s", esp_err_to_name(close_ret));
    }

    if (is_started) {
        // Stop the client to terminate the connection thread
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
    g_session_ready = false;
    
    // Clean up the health check task if it exists
    if (s_health_check_task_handle != NULL) {
        // Notify the health check task to exit by sending a notification
        if (s_health_check_task_handle != NULL) {
            xTaskNotify(s_health_check_task_handle, 1, eSetValueWithOverwrite);
            
            // Wait a bit for the task to terminate gracefully
            vTaskDelay(pdMS_TO_TICKS(100));
            
            // Remove from WDT if still registered
            esp_task_wdt_delete(s_health_check_task_handle);
        }
        
        // Reset the handle
        s_health_check_task_handle = NULL;
    }

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

esp_err_t websocket_client_send_audio(const uint8_t *data, size_t length, uint32_t timeout_ms) {
    // CRITICAL: Validate connection before sending
    if (!is_connected || g_ws_client == NULL) {
        ESP_LOGE(TAG, "Cannot send audio - not connected");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Additional validation to ensure connection is still alive
    if (!esp_websocket_client_is_connected(g_ws_client)) {
        ESP_LOGE(TAG, "WebSocket connection lost - cannot send audio");
        is_connected = false;
        return ESP_ERR_INVALID_STATE;
    }
    
    if (data == NULL || length == 0) {
        ESP_LOGE(TAG, "Invalid audio data");
        return ESP_ERR_INVALID_ARG;
    }
    
    // CRITICAL IMPROVEMENT: Implement exponential backoff for retries on send failures
    static uint32_t s_send_failure_count = 0;
    const uint32_t max_retries = 3;
    esp_err_t last_error = ESP_OK;
    
    for (uint32_t attempt = 0; attempt <= max_retries; attempt++) {
        // Re-check connection status before each send attempt
        if (!esp_websocket_client_is_connected(g_ws_client)) {
            ESP_LOGE(TAG, "WebSocket connection lost before send attempt %u", (unsigned int)(attempt + 1));
            is_connected = false;
            return ESP_ERR_INVALID_STATE;
        }
        
        // Calculate timeout with exponential backoff
        uint32_t effective_timeout = timeout_ms;
        if (attempt > 0) {
            // Exponential backoff: base timeout * 2^attempt
            effective_timeout = timeout_ms * (1 << attempt);
            if (effective_timeout > (timeout_ms * 8)) {  // Cap at 8x base timeout
                effective_timeout = timeout_ms * 8;
            }
            
            // Add jitter to prevent thundering herd
            uint32_t jitter = esp_random() % (effective_timeout / 4); // Up to 25% jitter
            effective_timeout += jitter;
            
            ESP_LOGD(TAG, "WebSocket send attempt %u with backoff timeout: %u ms (base: %u ms)", 
                     (unsigned int)(attempt + 1), (unsigned int)effective_timeout, (unsigned int)timeout_ms);
        }
        
        int ret = esp_websocket_client_send_bin(g_ws_client, (const char *)data, length, pdMS_TO_TICKS(effective_timeout));
        
        // CRITICAL FIX: Handle the case where send returns 0 (socket buffer full, need to wait)
        if (ret == 0) {
            ESP_LOGW(TAG, "WebSocket send buffer full (0 bytes sent), yielding and retrying (attempt %u/%u)", 
                     (unsigned int)(attempt + 1), (unsigned int)(max_retries + 1));
            // Yield to allow TCP/IP stack to drain send buffer
            vTaskDelay(pdMS_TO_TICKS(100));
            // Don't count this as a failure, just retry
            if (attempt == max_retries) {
                ESP_LOGE(TAG, "WebSocket send buffer remained full after %u attempts", (unsigned int)(max_retries + 1));
                last_error = ESP_ERR_TIMEOUT;
                s_send_failure_count++;
            }
            continue;  // Retry without incrementing failure counter
        }
        
        if (ret >= 0) {
            // Success - reset failure counter
            s_send_failure_count = 0;
            
            // Log success for first few attempts to verify reliability
            static uint32_t s_success_count = 0;
            s_success_count++;
            if (s_success_count <= 10 || (s_success_count % 100) == 0) {
                ESP_LOGD(TAG, "Sent %zu bytes of audio data (attempt: %u, success: %u)", 
                         length, (unsigned int)(attempt + 1), (unsigned int)s_success_count);
            }
            
            return ESP_OK;
        }
        
        // ret < 0 indicates an actual error
        last_error = ESP_FAIL;
        s_send_failure_count++;
        
        if (attempt < max_retries) {
            ESP_LOGW(TAG, "WebSocket send attempt %u failed (%d), retrying...", 
                     (unsigned int)(attempt + 1), ret);
            // Exponential backoff delay: 50ms, 100ms, 200ms
            vTaskDelay(pdMS_TO_TICKS(50 * (1 << attempt)));
            
            // Re-check connection status after delay
            if (!esp_websocket_client_is_connected(g_ws_client)) {
                ESP_LOGE(TAG, "WebSocket connection lost during retry");
                is_connected = false;
                return ESP_ERR_INVALID_STATE;
            }
        }
    }
    
    ESP_LOGE(TAG, "Failed to send audio chunk (%zu bytes) after %u attempts", 
             length, (unsigned int)(max_retries + 1));
    return last_error;
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

bool websocket_client_session_ready(void) {
    return is_connected && g_session_ready;
}

bool websocket_client_can_stream_audio(void) {
    websocket_pipeline_stage_t stage = g_pipeline_stage;
    if (!is_connected || !g_session_ready) {
        return false;
    }

    return stage == WEBSOCKET_PIPELINE_STAGE_IDLE ||
        stage == WEBSOCKET_PIPELINE_STAGE_TRANSCRIPTION ||
        stage == WEBSOCKET_PIPELINE_STAGE_COMPLETE;
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
            g_session_ready = false;
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
            g_session_ready = false;
            is_started = false;
            
            // CRITICAL FIX: Notify all components about disconnection to prevent hanging
            if (g_audio_callback) {
                // Send zero-length data to signal disconnection
                g_audio_callback(NULL, 0, g_audio_callback_arg);
            }
            
            // CRITICAL FIX: Also notify about disconnection status
            if (g_status_callback) {
                g_status_callback(WEBSOCKET_STATUS_DISCONNECTED, g_status_callback_arg);
            }
            
            // CRITICAL FIX: Trigger automatic reconnection after a brief delay
            // This helps maintain connection without manual intervention
            // Only create task if it doesn't already exist to prevent multiple attempts
            if (s_reconnect_task_handle == NULL) {
                BaseType_t ret = xTaskCreate(websocket_reconnect_task, "ws_reconnect_task", 2048, NULL, TASK_PRIORITY_WEBSOCKET - 1, &s_reconnect_task_handle);
                if (ret != pdPASS) {
                    ESP_LOGE(TAG, "Failed to create reconnect task");
                    s_reconnect_task_handle = NULL;
                }
            } else {
                ESP_LOGD(TAG, "Reconnect task already exists, skipping creation");
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
            is_connected = false; // CRITICAL: Mark as disconnected on error
            g_pipeline_stage = WEBSOCKET_PIPELINE_STAGE_IDLE;
            g_session_ready = false;
            is_started = false; // CRITICAL: Mark as not started
            
            // CRITICAL FIX: Notify all components about error to prevent hanging
            if (g_audio_callback) {
                // Send zero-length data to signal error/disconnection
                g_audio_callback(NULL, 0, g_audio_callback_arg);
            }
            
            // CRITICAL FIX: Also notify about the specific error
            if (g_status_callback) {
                g_status_callback(WEBSOCKET_STATUS_ERROR, g_status_callback_arg);
            }
            
            // CRITICAL FIX: Trigger reconnection attempt after a short delay
            // This prevents immediate retry loops that can overwhelm the system
            // Only create task if it doesn't already exist to prevent multiple attempts
            if (s_delayed_reconnect_task_handle == NULL) {
                BaseType_t ret = xTaskCreate(websocket_delayed_reconnect_task, "ws_delayed_reconnect_task", 2048, NULL, TASK_PRIORITY_WEBSOCKET - 1, &s_delayed_reconnect_task_handle);
                if (ret != pdPASS) {
                    ESP_LOGE(TAG, "Failed to create delayed reconnect task");
                    s_delayed_reconnect_task_handle = NULL;
                }
            } else {
                ESP_LOGD(TAG, "Delayed reconnect task already exists, skipping creation");
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
    json_str = NULL;  // Set to NULL after freeing to prevent double-free
    
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
        
        // ✅ PRIORITY 2: Handle server acknowledgment messages for flow control
        // Server sends {"status": "receiving", "chunks_received": N, "bytes_received": M} every 2 chunks
        // This allows us to implement backpressure and prevent send buffer overflow
        if (strcmp(status_str, "receiving") == 0) {
            cJSON *chunks_received = cJSON_GetObjectItem(root, "chunks_received");
            if (chunks_received != NULL && cJSON_IsNumber(chunks_received)) {
                uint32_t ack_chunk = (uint32_t)chunks_received->valueint;
                stt_pipeline_update_flow_control(ack_chunk);
                ESP_LOGI(TAG, "Server ACK: %u chunks processed", (unsigned int)ack_chunk);
            }
        }
        
        // Check if this is an error message indicating empty transcription
        if (strcmp(status_str, "error") == 0) {
            cJSON *message = cJSON_GetObjectItem(root, "message");
            if (message != NULL && cJSON_IsString(message)) {
                const char *message_str = message->valuestring;
                if (message_str != NULL && strstr(message_str, "Could not understand audio") != NULL) {
                    ESP_LOGW(TAG, "Received empty transcription error: %s", message_str);
                    
                    // Play audio feedback to indicate the empty transcription to the user
                    esp_err_t feedback_ret = audio_feedback_beep_triple(false); // Using triple beep for error feedback
                    if (feedback_ret != ESP_OK) {
                        ESP_LOGE(TAG, "Failed to play audio feedback for empty transcription: %s", esp_err_to_name(feedback_ret));
                    }
                }
            }
        }
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

// Session tracking variables - moved to file scope to fix scoping issues
static uint32_t s_total_bytes_received = 0;
static uint32_t s_message_count = 0;
static bool s_first_message_logged = false;
static uint32_t s_session_start_timestamp = 0; // Moved to file scope to fix undefined reference issue
static bool s_current_session_active = false;
static uint32_t s_current_session_bytes = 0;
static uint32_t s_session_message_count = 0;
static bool s_session_ended = false; // Track if current session has ended

// Missing handle_binary_message function implementation
static void handle_binary_message(const uint8_t *data, size_t len) {
    // CRITICAL FIX: Handle zero-length binary frames as end-of-stream signal
    // Server sends zero-length binary frame after all audio chunks to signal completion
    if (len == 0) {
        ESP_LOGI(TAG, "✅ Received end-of-audio signal (zero-length binary frame)");
        
        // Notify TTS decoder about end of stream
        tts_decoder_notify_end_of_stream();
        
        // Call audio callback with NULL data to signal end of session
        if (g_audio_callback) {
            g_audio_callback(NULL, 0, g_audio_callback_arg);
        }
        
        // Reset session tracking
        s_current_session_active = false;
        s_session_ended = true;
        
        ESP_LOGI(TAG, "🎵 Audio session complete: %u bytes in %u messages", 
                 (unsigned int)s_current_session_bytes, (unsigned int)s_session_message_count);
        
        s_current_session_bytes = 0;
        s_session_message_count = 0;
        return;
    }
    
    // CRITICAL FIX: Handle NULL data pointer (rare edge case)
    if (data == NULL) {
        ESP_LOGE(TAG, "Invalid binary message: NULL data pointer with non-zero length (%zu)", len);
        return;
    }
    
    s_total_bytes_received += len;
    s_message_count++;
    s_current_session_bytes += len;
    s_session_message_count++;
    
    // Log first few messages for debugging
    if (!s_first_message_logged || s_message_count <= 5) {
        ESP_LOGI(TAG, "Received binary audio data: %zu bytes (msg: %u, total: %u)", 
                 len, (unsigned int)s_message_count, (unsigned int)s_total_bytes_received);
        if (s_message_count == 5) {
            s_first_message_logged = true;
        }
    } else if ((s_message_count % 100) == 0) {  // Log every 100 messages to prevent log spam
        ESP_LOGD(TAG, "Received binary audio data: %zu bytes (msg: %u, total: %u)", 
                 len, (unsigned int)s_message_count, (unsigned int)s_total_bytes_received);
    }
    
    // Track when a new audio session starts
    if (!s_current_session_active || s_session_ended) {
        s_session_start_timestamp = (uint32_t)(esp_timer_get_time() / 1000);
        ESP_LOGI(TAG, "🎙️ New audio session started (timestamp: %u ms, bytes: %u)", 
                 (unsigned int)s_session_start_timestamp, (unsigned int)s_current_session_bytes);
        s_current_session_active = true;
        s_session_ended = false;
        s_current_session_bytes = len;  // Reset counter for new session
        s_session_message_count = 1;    // Reset message counter for new session
        
        // REMOVED: Silence chunk priming was interfering with WAV header parsing
        // The TTS decoder expects the first bytes to be a valid WAV RIFF header, not zeros
    }
    
    // Call audio callback if registered
    if (g_audio_callback) {
        g_audio_callback(data, len, g_audio_callback_arg);
    } else {
        ESP_LOGW(TAG, "No audio callback registered (msg: %u) - audio data discarded", (unsigned int)s_message_count);
        
        // Try to start TTS decoder if it's not running but should be
        if (g_session_ready && g_pipeline_stage == WEBSOCKET_PIPELINE_STAGE_TTS) {
            ESP_LOGI(TAG, "Audio data received but no callback - attempting to start TTS decoder");
            esp_err_t ret = tts_decoder_start();
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "TTS decoder started successfully for audio streaming");
                // Now try again with the callback
                if (g_audio_callback != NULL) {
                    g_audio_callback(data, len, g_audio_callback_arg);
                }
            } else {
                ESP_LOGE(TAG, "Failed to start TTS decoder: %s", esp_err_to_name(ret));
            }
        }
    }
    
    // REMOVED: Automatic session end detection based on chunk size
    // This was causing premature EOS signaling when the server sent small chunks
    // The server will explicitly signal completion via the "complete" status message
    // which is handled in handle_text_message() -> on_pipeline_stage_complete()
    
    // Log small chunks for debugging but don't treat them as session end
    if (len < 1024 && s_current_session_bytes > 4096) {
        ESP_LOGD(TAG, "Received small chunk (%zu bytes) in session (%u bytes total, %u messages)", 
                 len, (unsigned int)s_current_session_bytes, (unsigned int)s_session_message_count);
    }
    
    // CRITICAL FIX: Send periodic watchdog resets during long audio sessions
    static uint32_t s_watchdog_reset_counter = 0;
    s_watchdog_reset_counter++;
    if ((s_watchdog_reset_counter % 50) == 0) { // Reset watchdog every 50 messages
        // Reset watchdog to prevent system reset during long audio sessions
        safe_task_wdt_reset();
        ESP_LOGD(TAG, "Resetting watchdog (counter=%u, session_bytes=%u)", 
                 (unsigned int)s_watchdog_reset_counter, (unsigned int)s_current_session_bytes);
    }
}

static void update_session_ready_from_stage(websocket_pipeline_stage_t stage)
{
    switch (stage) {
        case WEBSOCKET_PIPELINE_STAGE_IDLE:
        case WEBSOCKET_PIPELINE_STAGE_COMPLETE:
            g_session_ready = true;
            break;
        case WEBSOCKET_PIPELINE_STAGE_TRANSCRIPTION:
            // Allow additional audio while server confirms transcription stage
            g_session_ready = true;
            break;
        default:
            g_session_ready = false;
            break;
    }
}

static void update_pipeline_stage(const char *status, const char *stage) {
    if (status == NULL) {
        return;
    }

    websocket_pipeline_stage_t new_stage = g_pipeline_stage;
    bool explicit_ready = g_session_ready;

    if (strcmp(status, "complete") == 0) {
        new_stage = WEBSOCKET_PIPELINE_STAGE_COMPLETE;
        explicit_ready = true;
    } else if (strcmp(status, "processing") == 0) {
        if (stage != NULL) {
            if (strcmp(stage, "transcription") == 0) {
                new_stage = WEBSOCKET_PIPELINE_STAGE_TRANSCRIPTION;
                explicit_ready = true;
            } else if (strcmp(stage, "llm") == 0) {
                new_stage = WEBSOCKET_PIPELINE_STAGE_LLM;
                explicit_ready = false;
            } else if (strcmp(stage, "tts") == 0) {
                new_stage = WEBSOCKET_PIPELINE_STAGE_TTS;
                explicit_ready = false;
            }
        }
    } else if (strcmp(status, "connected") == 0) {
        new_stage = WEBSOCKET_PIPELINE_STAGE_IDLE;
        explicit_ready = true;
    } else if (strcmp(status, "idle") == 0) {
        new_stage = WEBSOCKET_PIPELINE_STAGE_IDLE;
        explicit_ready = true;
    } else if (strcmp(status, "error") == 0) {
        explicit_ready = false;
    }

    // Handle transition cleanup
    bool stage_changed = (new_stage != g_pipeline_stage);
    if (stage_changed) {
        ESP_LOGI(TAG, "Pipeline stage changed: %s -> %s",
                 pipeline_stage_to_string(g_pipeline_stage),
                 pipeline_stage_to_string(new_stage));
        
        // Provide audio + LED feedback when entering processing stages
        // This informs user that system is working and they should wait
        if (new_stage == WEBSOCKET_PIPELINE_STAGE_TRANSCRIPTION && 
            g_pipeline_stage != WEBSOCKET_PIPELINE_STAGE_TRANSCRIPTION) {
            ESP_LOGI(TAG, "Entering TRANSCRIPTION stage - playing processing feedback");
            esp_err_t fb_ret = feedback_player_play(FEEDBACK_SOUND_PROCESSING);
            if (fb_ret != ESP_OK) {
                ESP_LOGW(TAG, "Transcription stage feedback failed: %s", esp_err_to_name(fb_ret));
            }
            // Set LED to pulsing to indicate processing
            led_controller_set_state(LED_STATE_PULSING);
        }
        
        // Continue pulsing LED during LLM processing
        if (new_stage == WEBSOCKET_PIPELINE_STAGE_LLM && 
            g_pipeline_stage != WEBSOCKET_PIPELINE_STAGE_LLM) {
            ESP_LOGI(TAG, "Entering LLM stage - continuing processing indication");
            led_controller_set_state(LED_STATE_PULSING);
        }
        
        // Special handling for TTS stage transitions
        if (g_pipeline_stage == WEBSOCKET_PIPELINE_STAGE_TTS && 
            new_stage != WEBSOCKET_PIPELINE_STAGE_TTS) {
            ESP_LOGI(TAG, "Exiting TTS stage (transition to %s)", 
                     pipeline_stage_to_string(new_stage));
            // ✅ FIX: Don't signal EOS on "complete" status - let TTS decoder 
            // detect completion naturally when all audio data received.
            // The "complete" status arrives before all chunks are transmitted,
            // causing premature session reset while audio still in flight.
            
            // Give TTS decoder time to finish processing buffered data
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        
        // Special handling when entering TTS stage
        if (new_stage == WEBSOCKET_PIPELINE_STAGE_TTS && 
            g_pipeline_stage != WEBSOCKET_PIPELINE_STAGE_TTS) {
            ESP_LOGI(TAG, "Entering TTS stage - preparing for audio streaming");
            
            // Set LED to breathing pattern to indicate audio is incoming
            // This provides visual cue that response playback is starting
            led_controller_set_state(LED_STATE_BREATHING);
            
            // CRITICAL FIX: Always try to start TTS decoder when entering TTS stage
            // This ensures audio playback works even if user exits voice mode prematurely
            // The TTS decoder will buffer audio and play it when I2S is available
            system_state_t current_state = state_manager_get_state();
            
            // Always attempt to start TTS decoder for incoming audio
            esp_err_t ret = tts_decoder_start();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to start TTS decoder for streaming: %s", esp_err_to_name(ret));
            }
            
            // Log warning if we're not in voice mode anymore
            if (current_state != SYSTEM_STATE_VOICE_ACTIVE) {
                ESP_LOGW(TAG, "TTS audio arriving after voice mode exit (state=%d) - will attempt playback", current_state);
            }
        }
        
        // Restore LED state when pipeline completes
        // User should see breathing LED (same as camera standby mode)
        if (new_stage == WEBSOCKET_PIPELINE_STAGE_COMPLETE && 
            g_pipeline_stage != WEBSOCKET_PIPELINE_STAGE_COMPLETE) {
            ESP_LOGI(TAG, "Pipeline complete - checking system state for LED restore");
            system_state_t current_state = state_manager_get_state();
            if (current_state == SYSTEM_STATE_VOICE_ACTIVE) {
                // Still in voice mode - restore solid LED (ready for next input)
                led_controller_set_state(LED_STATE_SOLID);
            } else if (current_state == SYSTEM_STATE_CAMERA_STANDBY) {
                // Back to camera mode - restore breathing LED
                led_controller_set_state(LED_STATE_BREATHING);
            }
        }
        
        // Cancel STT capture when moving to processing stages
        if (new_stage == WEBSOCKET_PIPELINE_STAGE_TRANSCRIPTION ||
            new_stage == WEBSOCKET_PIPELINE_STAGE_LLM ||
            new_stage == WEBSOCKET_PIPELINE_STAGE_TTS ||
            new_stage == WEBSOCKET_PIPELINE_STAGE_COMPLETE) {
            stt_pipeline_cancel_capture();
        }
        
        g_pipeline_stage = new_stage;
        post_pipeline_stage_event(new_stage);
    }

    if (explicit_ready != g_session_ready) {
        g_session_ready = explicit_ready;
    }

    update_session_ready_from_stage(g_pipeline_stage);
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

static void post_pipeline_stage_event(websocket_pipeline_stage_t stage)
{
    system_event_t evt = {
        .type = SYSTEM_EVENT_PIPELINE_STAGE,
        .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL),
        .data.pipeline = {
            .stage = stage,
        },
    };

    if (!event_dispatcher_post(&evt, pdMS_TO_TICKS(10))) {
        ESP_LOGW(TAG, "Failed to enqueue pipeline stage event (%s)",
                 pipeline_stage_to_string(stage));
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

// ===========================
// Helper Functions for WebSocket Reconnection Tasks
// ===========================

/**
 * @brief Task to handle WebSocket reconnection after a delay
 * 
 * This task waits for a specified delay and then attempts to reconnect
 * to the WebSocket server. It's used to prevent immediate retry loops
 * that can overwhelm the system.
 * 
 * @param pvParameters Not used
 */
static void websocket_reconnect_task(void *pvParameters) {
    vTaskDelay(pdMS_TO_TICKS(3000)); // Wait 3 seconds before reconnecting
    websocket_client_connect(); // Attempt reconnection
    
    // Reset the global handle since this task is being deleted
    s_reconnect_task_handle = NULL;
    
    vTaskDelete(NULL); // Self-delete
}

/**
 * @brief Task to handle delayed WebSocket reconnection
 * 
 * This task waits for a longer delay and then attempts to reconnect
 * to the WebSocket server. It's used after errors to prevent immediate
 * retry loops that can overwhelm the system.
 * 
 * @param pvParameters Not used
 */
static void websocket_delayed_reconnect_task(void *pvParameters) {
    vTaskDelay(pdMS_TO_TICKS(5000)); // Wait 5 seconds before reconnecting
    websocket_client_connect(); // Attempt reconnection
    
    // Reset the global handle since this task is being deleted
    s_delayed_reconnect_task_handle = NULL;
    
    vTaskDelete(NULL); // Self-delete
}



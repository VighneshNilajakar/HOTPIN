/**
 * @file main.c
 * @brief HotPin ESP32-CAM AI Agent - Main application entry point
 * 
 * Critical initialization sequence:
 * 1. Disable brownout detector
 * 2. GPIO 12 button input configuration (AVOIDS STRAPPING PIN CONFLICTS)
 * 3. PSRAM validation
 * 4. NVS and WiFi initialization
 * 5. Mutex and queue creation
 * 6. Module initialization
 * 7. FreeRTOS task spawning with proper core affinity
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_task_wdt.h"
#include "esp_psram.h"
#include "soc/rtc_cntl_reg.h"
#include "driver/rtc_io.h"

#include "config.h"
#include "button_handler.h"
#include "camera_controller.h"
#include "audio_driver.h"
#include "feedback_player.h"
#include "websocket_client.h"
#include "state_manager.h"
#include "stt_pipeline.h"
#include "tts_decoder.h"
#include "http_client.h"
#include "json_protocol.h"
#include "led_controller.h"
#include "serial_commands.h"
#include "event_dispatcher.h"
#include "system_events.h"
#include "memory_manager.h"

// ===========================
// Forward Declarations
// ===========================
static void websocket_status_callback(websocket_status_t status, void *arg);
static void websocket_connection_task(void *pvParameters);

// ===========================
// Global Variables
// ===========================
static const char *TAG = TAG_MAIN;

// Synchronization primitives (exported to modules)
SemaphoreHandle_t g_i2s_config_mutex = NULL;

// Task handles for coordination
TaskHandle_t g_state_manager_task_handle = NULL;
TaskHandle_t g_websocket_task_handle = NULL;

// Connection status flags
static volatile bool g_wifi_connected = false;
static volatile bool g_websocket_connected = false;

// WebSocket coordination
static EventGroupHandle_t g_network_event_group = NULL;

#define NETWORK_EVENT_WIFI_CONNECTED     BIT0
#define NETWORK_EVENT_WEBSOCKET_CONNECTED BIT1

// WiFi credentials from Kconfig (configured via .env file)
#define WIFI_SSID      CONFIG_HOTPIN_WIFI_SSID
#define WIFI_PASSWORD  CONFIG_HOTPIN_WIFI_PASSWORD

// WebSocket server URL from Kconfig (configured via .env file)
#define WS_SERVER_URI  CONFIG_WEBSOCKET_URI

// ===========================
// Forward Declarations
// ===========================
static esp_err_t validate_psram(void);
static esp_err_t init_nvs(void);
static esp_err_t init_wifi(void);
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data);
static void print_system_info(void);

// ===========================
// Main Application Entry
// ===========================

void app_main(void) {
    ESP_LOGI(TAG, "====================================");
    ESP_LOGI(TAG, "HotPin ESP32-CAM AI Agent Starting");
    ESP_LOGI(TAG, "====================================");
    
    // ===========================
    // Phase 1: Critical Hardware Initialization
    // ===========================
    
    // CRITICAL: Disable brownout detector for stability
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
    ESP_LOGW(TAG, "Brownout detector disabled");
    
    // NOTE: GPIO 12 is used for button input - configured in button_handler_init()
    // Flash LED on GPIO12 is NOT controlled to avoid conflicts with button
    
    // Validate PSRAM availability
    if (validate_psram() != ESP_OK) {
        ESP_LOGE(TAG, "PSRAM validation failed - ABORTING");
        esp_restart();
    }
    
    // Print system information
    print_system_info();
    
    // ===========================
    // Phase 2: Software Infrastructure
    // ===========================
    
    // Initialize memory manager with monitoring
    ESP_LOGI(TAG, "Initializing memory manager...");
    ESP_ERROR_CHECK(memory_manager_init(NULL));  // Use default thresholds
    ESP_ERROR_CHECK(memory_manager_start_monitoring(15000));  // Monitor every 15 seconds
    memory_manager_log_stats("System Boot");
    
    // Initialize NVS (required for WiFi)
    ESP_ERROR_CHECK(init_nvs());
    
    // Create synchronization primitives
    g_i2s_config_mutex = xSemaphoreCreateMutex();
    g_network_event_group = xEventGroupCreate();
    event_dispatcher_init();

    if (!g_i2s_config_mutex || !g_network_event_group ||
        event_dispatcher_queue() == NULL) {
        ESP_LOGE(TAG, "Failed to create synchronization primitives");
        esp_restart();
    }
    
    ESP_LOGI(TAG, "Synchronization primitives created");
    
    // ===========================
    // Phase 3: Network Initialization
    // ===========================
    
    ESP_ERROR_CHECK(init_wifi());
    
    // Wait for WiFi connection (with timeout)
    ESP_LOGI(TAG, "Waiting for WiFi connection...");
    vTaskDelay(pdMS_TO_TICKS(5000));  // 5 second timeout
    
    // ===========================
    // Phase 4: Module Initialization
    // ===========================
    
    ESP_LOGI(TAG, "Initializing button handler...");
    ESP_ERROR_CHECK(button_handler_init());
    
    // Serial command interface disabled to reduce UART contention during voice mode
    // ESP_LOGI(TAG, "Initializing serial command interface...");
    // ESP_ERROR_CHECK(serial_commands_init());
    
    ESP_LOGI(TAG, "Initializing LED controller...");
    ESP_ERROR_CHECK(led_controller_init());
    ESP_ERROR_CHECK(feedback_player_init());
    ESP_ERROR_CHECK(led_controller_set_state(LED_STATE_FAST_BLINK));
    
    ESP_LOGI(TAG, "Initializing WebSocket client...");
    ESP_ERROR_CHECK(websocket_client_init(WS_SERVER_URI, CONFIG_AUTH_BEARER_TOKEN));
    
    // Register WebSocket status callback
    websocket_client_set_status_callback(websocket_status_callback, NULL);
    ESP_LOGI(TAG, "WebSocket status callback registered");
    
    ESP_LOGI(TAG, "Initializing HTTP client...");
    ESP_ERROR_CHECK(http_client_init(CONFIG_HTTP_SERVER_URL, CONFIG_AUTH_BEARER_TOKEN));
    
    // Initialize STT and TTS pipelines
    ESP_LOGI(TAG, "Initializing STT pipeline...");
    ESP_ERROR_CHECK(stt_pipeline_init());
    
    ESP_LOGI(TAG, "Initializing TTS decoder...");
    ESP_ERROR_CHECK(tts_decoder_init());
    
    // Camera and audio drivers will be initialized by state manager
    ESP_LOGI(TAG, "Camera and audio initialization deferred to state manager");
    
    // ===========================
    // Phase 5: Task Creation
    // ===========================
    
    ESP_LOGI(TAG, "Creating FreeRTOS tasks...");
    
    // State Manager Task (Core 1, Highest Priority)
    BaseType_t ret = xTaskCreatePinnedToCore(
        state_manager_task,
        "state_mgr",
        TASK_STACK_SIZE_LARGE,
        NULL,
        TASK_PRIORITY_STATE_MANAGER,
        &g_state_manager_task_handle,
    TASK_CORE_CONTROL
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create state manager task");
        // Clean up synchronization primitives before restarting
        if (g_i2s_config_mutex) {
            vSemaphoreDelete(g_i2s_config_mutex);
            g_i2s_config_mutex = NULL;
        }
        if (g_network_event_group) {
            vEventGroupDelete(g_network_event_group);
            g_network_event_group = NULL;
        }
        esp_restart();
    }
    
    ESP_LOGI(TAG, "State manager task created on Core 1");
    
    // WebSocket Connection Management Task (Core 0, Medium Priority)
    ret = xTaskCreatePinnedToCore(
        websocket_connection_task,
        "ws_connect",
        TASK_STACK_SIZE_MEDIUM,
        NULL,
        TASK_PRIORITY_WEBSOCKET - 1,  // Lower priority than main WebSocket I/O
        &g_websocket_task_handle,
    TASK_CORE_NETWORK_IO
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create WebSocket connection task");
        // Clean up resources before restarting
        if (g_state_manager_task_handle) {
            vTaskDelete(g_state_manager_task_handle);
            g_state_manager_task_handle = NULL;
        }
        if (g_i2s_config_mutex) {
            vSemaphoreDelete(g_i2s_config_mutex);
            g_i2s_config_mutex = NULL;
        }
        if (g_network_event_group) {
            vEventGroupDelete(g_network_event_group);
            g_network_event_group = NULL;
        }
        esp_restart();
    }
    
    ESP_LOGI(TAG, "WebSocket connection task created on Core 0");
    
    // ===========================
    // Task Watchdog Configuration
    // ===========================
    // Note: The TWDT is automatically initialized by ESP-IDF startup code.
    // We just need to add our tasks to it, not reinitialize it.
    ESP_LOGI(TAG, "Configuring task watchdog for critical tasks...");
    
    // Add state manager task to WDT
    if (g_state_manager_task_handle != NULL) {
        ret = esp_task_wdt_add(g_state_manager_task_handle);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "✅ State manager task added to watchdog");
        } else {
            ESP_LOGW(TAG, "Failed to add state manager to watchdog: %s", 
                     esp_err_to_name(ret));
        }
    }
    
    // Add WebSocket connection task to WDT
    if (g_websocket_task_handle != NULL) {
        ret = esp_task_wdt_add(g_websocket_task_handle);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "✅ WebSocket connection task added to watchdog");
        } else {
            ESP_LOGW(TAG, "Failed to add WebSocket task to watchdog: %s", 
                     esp_err_to_name(ret));
        }
    }
    
    ESP_LOGI(TAG, "Task watchdog configuration complete");
    
    // WebSocket task now runs continuously; state manager handles camera/audio
    
    // ===========================
    // Phase 6: System Ready
    // ===========================
    
    ESP_LOGI(TAG, "====================================");
    ESP_LOGI(TAG, "System initialization complete!");
    ESP_LOGI(TAG, "Entering camera standby mode...");
    ESP_LOGI(TAG, "====================================");

    system_event_t boot_event = {
        .type = SYSTEM_EVENT_BOOT_COMPLETE,
        .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL),
    };
    if (!event_dispatcher_post(&boot_event, pdMS_TO_TICKS(10))) {
        ESP_LOGW(TAG, "Boot event drop (dispatcher not ready)");
    }

    esp_err_t boot_feedback_ret = feedback_player_play(FEEDBACK_SOUND_BOOT);
    if (boot_feedback_ret != ESP_OK) {
        ESP_LOGW(TAG, "Boot feedback playback failed: %s", esp_err_to_name(boot_feedback_ret));
    }

    ESP_ERROR_CHECK(led_controller_set_state(LED_STATE_BREATHING));
    
    // Main task complete - FreeRTOS scheduler continues
    ESP_LOGI(TAG, "Main task exiting - system running");
}

// ===========================
// Private Functions
// ===========================

// REMOVED: critical_gpio_init() function
// GPIO12 is used ONLY as button input (configured in button_handler_init)
// Note: GPIO12 is a strapping pin - ensure proper external pull-up/pull-down
// for reliable boot behavior

static esp_err_t validate_psram(void) {
    ESP_LOGI(TAG, "Validating PSRAM...");
    
    size_t psram_size = esp_psram_get_size();
    
    if (psram_size == 0) {
        ESP_LOGE(TAG, "PSRAM not detected!");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "PSRAM detected: %d bytes (%.2f MB)", 
             psram_size, psram_size / (1024.0 * 1024.0));
    
    if (psram_size < (4 * 1024 * 1024)) {
        ESP_LOGE(TAG, "PSRAM size < 4MB - insufficient for operation");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "PSRAM validation passed");
    return ESP_OK;
}

static esp_err_t init_nvs(void) {
    ESP_LOGI(TAG, "Initializing NVS...");
    
    esp_err_t ret = nvs_flash_init();
    
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs to be erased");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "NVS initialized successfully");
    }
    
    return ret;
}

static esp_err_t init_wifi(void) {
    ESP_LOGI(TAG, "Initializing WiFi...");
    
    // Initialize TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    
    // Initialize WiFi with default config
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, 
                                                &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, 
                                                &wifi_event_handler, NULL));
    
    // Configure WiFi as station mode
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_LOGI(TAG, "WiFi initialization complete, connecting to %s...", WIFI_SSID);
    
    return ESP_OK;
}

/**
 * @brief Callback for WebSocket connection status changes
 */
static void websocket_status_callback(websocket_status_t status, void *arg) {
    system_event_t evt = {
        .type = SYSTEM_EVENT_WEBSOCKET_STATUS,
        .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL),
        .data.websocket = {
            .status = status,
        },
    };

    if (!event_dispatcher_post(&evt, pdMS_TO_TICKS(10))) {
        ESP_LOGW(TAG, "WebSocket status event drop (queue full)");
    }

    switch (status) {
        case WEBSOCKET_STATUS_CONNECTED:
            ESP_LOGI(TAG, "🎉 WebSocket status callback: CONNECTED");
            g_websocket_connected = true;
            if (g_network_event_group != NULL) {
                xEventGroupSetBits(g_network_event_group, NETWORK_EVENT_WEBSOCKET_CONNECTED);
            }
            break;
            
        case WEBSOCKET_STATUS_DISCONNECTED:
            ESP_LOGW(TAG, "⚠️ WebSocket status callback: DISCONNECTED");
            g_websocket_connected = false;
            if (g_network_event_group != NULL) {
                xEventGroupClearBits(g_network_event_group, NETWORK_EVENT_WEBSOCKET_CONNECTED);
            }
            break;
            
        case WEBSOCKET_STATUS_ERROR:
            ESP_LOGE(TAG, "❌ WebSocket status callback: ERROR");
            g_websocket_connected = false;
            if (g_network_event_group != NULL) {
                xEventGroupClearBits(g_network_event_group, NETWORK_EVENT_WEBSOCKET_CONNECTED);
            }
            break;
            
        default:
            break;
    }
}

/**
 * @brief WebSocket connection management task
 * 
 * Maintains a persistent WebSocket connection while WiFi is available.
 * Automatically retries with exponential backoff when disconnected.
 */
static void websocket_connection_task(void *pvParameters) {
    const int MAX_RETRY_DELAY_MS = 30000;
    int retry_delay_ms = 5000;
    int attempt = 0;
    bool shutdown_logged = false;  // ✅ FIX #9: Prevent log spam during shutdown

    ESP_LOGI(TAG, "WebSocket connection task started on Core %d", xPortGetCoreID());

    while (true) {
        // Check for system shutdown state
        system_state_t current_state = state_manager_get_state();
        if (current_state == SYSTEM_STATE_SHUTDOWN || current_state == SYSTEM_STATE_ERROR) {
            if (!shutdown_logged) {
                ESP_LOGI(TAG, "System shutdown detected, terminating WebSocket connection task");
                shutdown_logged = true;
            }
            break;
        }

        xEventGroupWaitBits(
            g_network_event_group,
            NETWORK_EVENT_WIFI_CONNECTED,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

        retry_delay_ms = 5000;
        attempt = 0;

        while ((xEventGroupGetBits(g_network_event_group) & NETWORK_EVENT_WIFI_CONNECTED) != 0) {
            // Check for system shutdown state
            current_state = state_manager_get_state();
            if (current_state == SYSTEM_STATE_SHUTDOWN || current_state == SYSTEM_STATE_ERROR) {
                if (!shutdown_logged) {
                    ESP_LOGI(TAG, "System shutdown detected, terminating WebSocket connection task");
                    shutdown_logged = true;
                }
                break;
            }

            if (!websocket_client_is_connected()) {
                attempt++;
                ESP_LOGI(TAG, "🔌 Attempting WebSocket connection (attempt %d)...", attempt);
                esp_err_t connect_ret = websocket_client_connect();
                if (connect_ret != ESP_OK) {
                    ESP_LOGE(TAG, "❌ WebSocket connection failed: %s", esp_err_to_name(connect_ret));
                }
            }

            TickType_t start = xTaskGetTickCount();
            TickType_t poll_delay = pdMS_TO_TICKS(200);
            TickType_t wait_duration = pdMS_TO_TICKS(retry_delay_ms);

            while (!websocket_client_is_connected()) {
                // Check for system shutdown state
                current_state = state_manager_get_state();
                if (current_state == SYSTEM_STATE_SHUTDOWN || current_state == SYSTEM_STATE_ERROR) {
                    if (!shutdown_logged) {
                        ESP_LOGI(TAG, "System shutdown detected, terminating WebSocket connection task");
                        shutdown_logged = true;
                    }
                    break;
                }

                if ((xEventGroupGetBits(g_network_event_group) & NETWORK_EVENT_WIFI_CONNECTED) == 0) {
                    break;
                }

                if ((xTaskGetTickCount() - start) >= wait_duration) {
                    break;
                }

                vTaskDelay(poll_delay);
            }

            // Check for system shutdown state
            current_state = state_manager_get_state();
            if (current_state == SYSTEM_STATE_SHUTDOWN || current_state == SYSTEM_STATE_ERROR) {
                if (!shutdown_logged) {
                    ESP_LOGI(TAG, "System shutdown detected, terminating WebSocket connection task");
                    shutdown_logged = true;
                }
                break;
            }

            if (!websocket_client_is_connected()) {
                xEventGroupClearBits(g_network_event_group, NETWORK_EVENT_WEBSOCKET_CONNECTED);

                if ((xEventGroupGetBits(g_network_event_group) & NETWORK_EVENT_WIFI_CONNECTED) == 0) {
                    ESP_LOGW(TAG, "WiFi offline, waiting for reconnection");
                    break;
                }

                retry_delay_ms = retry_delay_ms + (retry_delay_ms / 2);
                if (retry_delay_ms > MAX_RETRY_DELAY_MS) {
                    retry_delay_ms = MAX_RETRY_DELAY_MS;
                }

                continue;
            }

            ESP_LOGI(TAG, "📡 WebSocket connection active - monitoring link");
            xEventGroupSetBits(g_network_event_group, NETWORK_EVENT_WEBSOCKET_CONNECTED);
            retry_delay_ms = 5000;

            // ✅ STABILITY FIX: Monitor connection health with shorter intervals
            // This allows faster detection of transport errors and prevents watchdog starvation
            int health_checks = 0;
            const int HEALTH_CHECK_INTERVAL_MS = 1000;  // Check every 1 second
            const int MAX_HEALTH_CHECKS = 30;  // 30 seconds before forced reconnect
            
            while (websocket_client_is_connected() &&
                   (xEventGroupGetBits(g_network_event_group) & NETWORK_EVENT_WIFI_CONNECTED) != 0) {
                // Check for system shutdown state
                current_state = state_manager_get_state();
                if (current_state == SYSTEM_STATE_SHUTDOWN || current_state == SYSTEM_STATE_ERROR) {
                    if (!shutdown_logged) {
                        ESP_LOGI(TAG, "System shutdown detected, terminating WebSocket connection task");
                        shutdown_logged = true;
                    }
                    break;
                }

                // Reset watchdog timer to prevent timeout
                // ✅ IMPROVED: Use safe version that suppresses benign errors
                esp_err_t wdt_ret = esp_task_wdt_reset();
                if (wdt_ret != ESP_OK && wdt_ret != ESP_ERR_NOT_FOUND && wdt_ret != ESP_ERR_INVALID_ARG) {
                    ESP_LOGD(TAG, "WDT reset failed: %s", esp_err_to_name(wdt_ret));
                }
                
                vTaskDelay(pdMS_TO_TICKS(HEALTH_CHECK_INTERVAL_MS));
                health_checks++;
                
                // ✅ STABILITY FIX: Force reconnect if connection appears stale
                // This prevents silent failures where websocket_client_is_connected() returns true
                // but the underlying transport has errors (e.g., transport_poll_write failures)
                if (health_checks >= MAX_HEALTH_CHECKS) {
                    ESP_LOGW(TAG, "⚠️ Connection health check timeout - forcing reconnect to prevent stale connection");
                    break;  // Exit monitoring loop to trigger reconnection
                }
            }

            // Check for system shutdown state
            current_state = state_manager_get_state();
            if (current_state == SYSTEM_STATE_SHUTDOWN || current_state == SYSTEM_STATE_ERROR) {
                if (!shutdown_logged) {
                    ESP_LOGI(TAG, "System shutdown detected, terminating WebSocket connection task");
                    shutdown_logged = true;
                }
                break;
            }

            // ✅ STABILITY FIX: Centralized reconnection logic
            // This is the ONLY place where reconnection should be initiated
            // Other tasks (state_manager, etc.) should only react to connection status
            ESP_LOGW(TAG, "⚠️ WebSocket link not healthy, initiating reconnection sequence");
            xEventGroupClearBits(g_network_event_group, NETWORK_EVENT_WEBSOCKET_CONNECTED);
            
            // Force stop the client to clean up any stale state
            websocket_client_force_stop();
            
            // Wait longer to ensure clean shutdown before reconnect attempt
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        
            // Check for system shutdown state
        current_state = state_manager_get_state();
        if (current_state == SYSTEM_STATE_SHUTDOWN || current_state == SYSTEM_STATE_ERROR) {
            if (!shutdown_logged) {
                ESP_LOGI(TAG, "System shutdown detected, terminating WebSocket connection task");
                shutdown_logged = true;
            }
            break;
        }
    }
    
    // Unregister from watchdog before task deletion
    ESP_LOGI(TAG, "Unregistering WebSocket connection task from watchdog");
    esp_err_t wdt_ret = esp_task_wdt_delete(NULL);  // NULL = current task
    if (wdt_ret != ESP_OK && wdt_ret != ESP_ERR_INVALID_ARG) {
        ESP_LOGW(TAG, "Failed to unregister ws_connect task from watchdog: %s", esp_err_to_name(wdt_ret));
    }
    
    ESP_LOGI(TAG, "WebSocket connection task terminated");
    vTaskDelete(NULL);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WiFi station started, connecting...");
                led_controller_set_state(LED_STATE_FAST_BLINK);
                if (g_network_event_group != NULL) {
                    xEventGroupClearBits(g_network_event_group,
                                         NETWORK_EVENT_WIFI_CONNECTED);
                    xEventGroupClearBits(g_network_event_group,
                                         NETWORK_EVENT_WEBSOCKET_CONNECTED);
                }
                esp_wifi_connect();
                break;
                
            case WIFI_EVENT_STA_DISCONNECTED:
                ESP_LOGW(TAG, "WiFi disconnected, retrying...");
                g_wifi_connected = false;
                g_websocket_connected = false;
                led_controller_set_state(LED_STATE_FAST_BLINK);
                if (g_network_event_group != NULL) {
                    xEventGroupClearBits(g_network_event_group, NETWORK_EVENT_WIFI_CONNECTED);
                    xEventGroupClearBits(g_network_event_group, NETWORK_EVENT_WEBSOCKET_CONNECTED);
                }
                esp_wifi_connect();
                break;
                
            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "WiFi connected to AP");
                break;
                
            default:
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "✅ Got IP address: " IPSTR, IP2STR(&event->ip_info.ip));
        
        // Set WiFi connected flag
        g_wifi_connected = true;
        if (state_manager_get_state() != SYSTEM_STATE_VOICE_ACTIVE) {
            led_controller_set_state(LED_STATE_BREATHING);
        }
        if (g_network_event_group != NULL) {
            xEventGroupSetBits(g_network_event_group, NETWORK_EVENT_WIFI_CONNECTED);
        }
        
        // WebSocket connection will be handled by websocket_connection_task
        ESP_LOGI(TAG, "WiFi ready - WebSocket connection task will handle server connection");
    }
}

static void print_system_info(void) {
    ESP_LOGI(TAG, "====================================");
    ESP_LOGI(TAG, "System Information:");
    
    // Chip info
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    
    ESP_LOGI(TAG, "Chip: %s", CONFIG_IDF_TARGET);
    ESP_LOGI(TAG, "Cores: %d", chip_info.cores);
    ESP_LOGI(TAG, "Silicon revision: %d", chip_info.revision);
    ESP_LOGI(TAG, "CPU Frequency: %d MHz", CONFIG_ESP32_DEFAULT_CPU_FREQ_MHZ);
    
    // Flash info
    uint32_t flash_size;
    esp_flash_get_size(NULL, &flash_size);
    ESP_LOGI(TAG, "Flash: %lu MB %s", flash_size / (1024 * 1024),
             (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");
    
    // Memory info
    ESP_LOGI(TAG, "Free heap: %lu bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "PSRAM: %zu bytes", esp_psram_get_size());
    
    ESP_LOGI(TAG, "====================================");
}

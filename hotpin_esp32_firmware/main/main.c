/**
 * @file main.c
 * @brief HotPin ESP32-CAM AI Agent - Main application entry point
 * 
 * Critical initialization sequence:
 * 1. Disable brownout detector
 * 2. GPIO 4 LED control (PREVENTS GHOST FLASH)
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
#include "freertos/queue.h"
#include "freertos/semphr.h"
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
#include "soc/rtc_cntl_reg.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"

#include "config.h"
#include "button_handler.h"
#include "camera_controller.h"
#include "audio_driver.h"
#include "websocket_client.h"
#include "state_manager.h"
#include "stt_pipeline.h"
#include "tts_decoder.h"
#include "http_client.h"
#include "json_protocol.h"
#include "led_controller.h"

// ===========================
// Global Variables
// ===========================
static const char *TAG = TAG_MAIN;

// Synchronization primitives (exported to modules)
SemaphoreHandle_t g_i2s_config_mutex = NULL;
QueueHandle_t g_button_event_queue = NULL;
QueueHandle_t g_state_event_queue = NULL;

// Task handles for coordination
TaskHandle_t g_state_manager_task_handle = NULL;
TaskHandle_t g_websocket_task_handle = NULL;

// WiFi credentials (TODO: Move to NVS or menuconfig)
#define WIFI_SSID      "SGF14"
#define WIFI_PASSWORD  "12345678vn"

// WebSocket server URL
#define WS_SERVER_URI  "ws://10.95.252.58:8000/ws"

// ===========================
// Forward Declarations
// ===========================
static void critical_gpio_init(void);
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
    
    // NOTE: GPIO 4 is used for button input - configured in button_handler_init()
    // Flash LED on GPIO4 is NOT controlled to avoid conflicts with button
    
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
    
    // Initialize NVS (required for WiFi)
    ESP_ERROR_CHECK(init_nvs());
    
    // Create synchronization primitives
    g_i2s_config_mutex = xSemaphoreCreateMutex();
    g_button_event_queue = xQueueCreate(10, sizeof(button_event_t));
    g_state_event_queue = xQueueCreate(20, sizeof(state_event_t));
    
    if (!g_i2s_config_mutex || !g_button_event_queue || !g_state_event_queue) {
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
    ESP_ERROR_CHECK(button_handler_init(g_button_event_queue));
    
    ESP_LOGI(TAG, "Initializing LED controller...");
    ESP_ERROR_CHECK(led_controller_init());
    led_controller_set_state(LED_STATE_WIFI_CONNECTING);
    
    ESP_LOGI(TAG, "Initializing WebSocket client...");
    ESP_ERROR_CHECK(websocket_client_init(WS_SERVER_URI, CONFIG_AUTH_BEARER_TOKEN));
    
    ESP_LOGI(TAG, "Initializing HTTP client...");
    ESP_ERROR_CHECK(http_client_init(CONFIG_HTTP_SERVER_URL, CONFIG_AUTH_BEARER_TOKEN));
    
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
        TASK_CORE_APP
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create state manager task");
        esp_restart();
    }
    
    ESP_LOGI(TAG, "State manager task created on Core 1");
    
    // Initialize task watchdog (30 second timeout)
    ESP_LOGI(TAG, "Initializing task watchdog (30s timeout)...");
    esp_task_wdt_init(30, true);  // 30 seconds, panic on timeout
    
    // Subscribe state manager task
    if (g_state_manager_task_handle != NULL) {
        ret = esp_task_wdt_add(g_state_manager_task_handle);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "State manager task added to watchdog");
        } else {
            ESP_LOGW(TAG, "Failed to add state manager to watchdog: %s", 
                     esp_err_to_name(ret));
        }
    }
    
    // WebSocket task will be started by state manager when needed
    // Audio and camera tasks are managed by state manager
    
    // ===========================
    // Phase 6: System Ready
    // ===========================
    
    ESP_LOGI(TAG, "====================================");
    ESP_LOGI(TAG, "System initialization complete!");
    ESP_LOGI(TAG, "Entering camera standby mode...");
    ESP_LOGI(TAG, "====================================");
    
    // Configure status LED (GPIO 2) to indicate system ready
    gpio_set_direction(CONFIG_STATUS_LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(CONFIG_STATUS_LED_GPIO, 1);  // LED ON = System ready
    
    // Main task complete - FreeRTOS scheduler continues
    ESP_LOGI(TAG, "Main task exiting - system running");
}

// ===========================
// Private Functions
// ===========================

// REMOVED: critical_gpio_init() function
// GPIO4 is used ONLY as button input (configured in button_handler_init)
// The onboard flash LED on GPIO4 may flicker but this is acceptable
// to maintain button functionality

static esp_err_t validate_psram(void) {
    ESP_LOGI(TAG, "Validating PSRAM...");
    
    size_t psram_size = esp_spiram_get_size();
    
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

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WiFi station started, connecting...");
                esp_wifi_connect();
                break;
                
            case WIFI_EVENT_STA_DISCONNECTED:
                ESP_LOGW(TAG, "WiFi disconnected, retrying...");
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
        ESP_LOGI(TAG, "Got IP address: " IPSTR, IP2STR(&event->ip_info.ip));
        led_controller_set_state(LED_STATE_WIFI_CONNECTED);
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
    ESP_LOGI(TAG, "PSRAM: %lu bytes", esp_spiram_get_size());
    
    ESP_LOGI(TAG, "====================================");
}

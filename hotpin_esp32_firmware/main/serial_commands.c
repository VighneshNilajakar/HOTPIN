/**
 * @file serial_commands.c
 * @brief Serial command interface implementation
 */

#include "serial_commands.h"
#include "button_handler.h"
#include "config.h"
#include "event_dispatcher.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "SERIAL_CMD";
static TaskHandle_t g_serial_task_handle = NULL;
static bool g_running = false;

// UART configuration
#define UART_NUM            UART_NUM_0
#define UART_BUF_SIZE       256
#define UART_RX_TIMEOUT_MS  20

static void print_help(void) {
    printf("\n");
    printf("========================================\n");
    printf("  HotPin Serial Command Interface\n");
    printf("========================================\n");
    printf("Commands:\n");
    printf("  s - Toggle voice recording (start/stop)\n");
    printf("  c - Capture image\n");
    printf("  l - Long press (shutdown simulation)\n");
    printf("  d - Toggle debug mode\n");
    printf("  h - Show this help\n");
    printf("========================================\n");
    printf("\n");
}

static void serial_command_task(void *pvParameters) {
    uint8_t data[UART_BUF_SIZE];
    bool voice_active = false;
    
    ESP_LOGI(TAG, "Serial command task started on Core %d", xPortGetCoreID());
    print_help();
    
    while (g_running) {
        // Read data from UART
        int len = uart_read_bytes(UART_NUM, data, 1, pdMS_TO_TICKS(100));
        
        if (len > 0) {
            char cmd = (char)data[0];
            
            // Convert to lowercase
            if (cmd >= 'A' && cmd <= 'Z') {
                cmd = cmd + ('a' - 'A');
            }
            
            system_event_t evt = {
                .type = SYSTEM_EVENT_BUTTON_INPUT,
                .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL),
            };

            switch (cmd) {
                case 's':
                    // Toggle voice mode (simulate single click)
                    voice_active = !voice_active;
                    evt.data.button.type = BUTTON_EVENT_SINGLE_CLICK;
                    evt.data.button.duration_ms = 0;

                    if (event_dispatcher_post(&evt, pdMS_TO_TICKS(10))) {
                        if (voice_active) {
                            printf("📢 Voice mode STARTED (recording...)\n");
                            ESP_LOGI(TAG, "Simulated SHORT PRESS - Voice START");
                        } else {
                            printf("🔇 Voice mode STOPPED\n");
                            ESP_LOGI(TAG, "Simulated SHORT PRESS - Voice STOP");
                        }
                    } else {
                        ESP_LOGW(TAG, "Failed to send button event (queue full)");
                    }
                    break;
                    
                case 'c':
                    // Capture image (simulate double click)
                    evt.data.button.type = BUTTON_EVENT_DOUBLE_CLICK;
                    evt.data.button.duration_ms = 0;

                    if (event_dispatcher_post(&evt, pdMS_TO_TICKS(10))) {
                        printf("📷 Image capture triggered!\n");
                        ESP_LOGI(TAG, "Simulated DOUBLE CLICK - Camera capture");
                    } else {
                        ESP_LOGW(TAG, "Failed to send button event (queue full)");
                    }
                    break;
                    
                case 'l':
                    // Long press (simulate shutdown)
                    evt.data.button.type = BUTTON_EVENT_LONG_PRESS;
                    evt.data.button.duration_ms = 0;

                    if (event_dispatcher_post(&evt, pdMS_TO_TICKS(10))) {
                        printf("🔴 Long press - Shutdown simulated\n");
                        ESP_LOGI(TAG, "Simulated LONG PRESS - Shutdown");
                    } else {
                        ESP_LOGW(TAG, "Failed to send button event (queue full)");
                    }
                    break;
                    
                case 'd':
                    // Toggle debug mode (future use)
                    printf("🔧 Debug mode toggle (not implemented yet)\n");
                    ESP_LOGI(TAG, "Debug toggle command");
                    break;
                    
                case 'h':
                case '?':
                    // Show help
                    print_help();
                    break;
                    
                case '\r':
                case '\n':
                    // Ignore newlines
                    break;
                    
                default:
                    printf("❌ Unknown command '%c'. Press 'h' for help.\n", cmd);
                    break;
            }
        }
        
        // Small delay to prevent task hogging
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    ESP_LOGI(TAG, "Serial command task exiting");
    vTaskDelete(NULL);
}

esp_err_t serial_commands_init(void) {
    if (event_dispatcher_queue() == NULL) {
        ESP_LOGE(TAG, "Event dispatcher queue not ready");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Initializing serial command interface...");

    // Configure UART (usually already configured by bootloader, but ensure settings)
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    // Install UART driver (if not already installed)
    esp_err_t ret = uart_driver_install(UART_NUM, UART_BUF_SIZE * 2, 0, 0, NULL, 0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "UART driver install failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = uart_param_config(UART_NUM, &uart_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UART config failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Create serial command task
    g_running = true;
    
    BaseType_t task_ret = xTaskCreatePinnedToCore(
        serial_command_task,
        "serial_cmd",
        TASK_STACK_SIZE_MEDIUM,
        NULL,
        TASK_PRIORITY_BUTTON_FSM,  // Same priority as button handler
        &g_serial_task_handle,
        TASK_CORE_AUDIO_IO  // Core 0 - I/O operations
    );
    
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create serial command task");
        g_running = false;
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "✅ Serial command interface initialized");
    return ESP_OK;
}

esp_err_t serial_commands_deinit(void) {
    ESP_LOGI(TAG, "Deinitializing serial command interface...");
    
    g_running = false;
    
    if (g_serial_task_handle != NULL) {
        vTaskDelay(pdMS_TO_TICKS(100));  // Wait for task to exit
        g_serial_task_handle = NULL;
    }
    
    uart_driver_delete(UART_NUM);
    
    ESP_LOGI(TAG, "Serial command interface deinitialized");
    return ESP_OK;
}

/**
 * @file led_controller.c
 * @brief LED controller implementation
 */

#include "led_controller.h"
#include "config.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

// ===========================
// Constants
// ===========================
#define LED_BLINK_SLOW_MS       500
#define LED_BLINK_FAST_MS       100
#define LED_PULSE_PERIOD_MS     2000
#define LED_PWM_CHANNEL         LEDC_CHANNEL_1
#define LED_PWM_TIMER           LEDC_TIMER_1
#define LED_PWM_DUTY_RES        LEDC_TIMER_8_BIT
#define LED_PWM_FREQUENCY       5000

// ===========================
// Private Variables
// ===========================
static const char *TAG = "LED_CTRL";
static TaskHandle_t led_task_handle = NULL;
static led_state_t current_state = LED_STATE_OFF;
static bool is_initialized = false;

// ===========================
// Forward Declarations
// ===========================
static void led_task(void *pvParameters);
static void set_led_level(uint8_t level);

// ===========================
// Public Functions
// ===========================

esp_err_t led_controller_init(void) {
    ESP_LOGI(TAG, "Initializing LED controller on GPIO %d", CONFIG_STATUS_LED_GPIO);
    
    // Configure GPIO as output
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << CONFIG_STATUS_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure GPIO: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Configure PWM for pulsing effect
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LED_PWM_TIMER,
        .duty_resolution = LED_PWM_DUTY_RES,
        .freq_hz = LED_PWM_FREQUENCY,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ret = ledc_timer_config(&ledc_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure LEDC timer: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ledc_channel_config_t ledc_channel = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LED_PWM_CHANNEL,
        .timer_sel = LED_PWM_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = CONFIG_STATUS_LED_GPIO,
        .duty = 0,
        .hpoint = 0
    };
    ret = ledc_channel_config(&ledc_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure LEDC channel: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Create LED task
    BaseType_t task_ret = xTaskCreatePinnedToCore(
        led_task,
        "led_task",
        TASK_STACK_SIZE_SMALL,
        NULL,
        1,  // Low priority
        &led_task_handle,
        TASK_CORE_PRO
    );
    
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create LED task");
        return ESP_ERR_NO_MEM;
    }
    
    is_initialized = true;
    set_led_level(0);  // Start with LED off
    ESP_LOGI(TAG, "LED controller initialized");
    return ESP_OK;
}

esp_err_t led_controller_deinit(void) {
    ESP_LOGI(TAG, "Deinitializing LED controller");
    
    if (led_task_handle) {
        vTaskDelete(led_task_handle);
        led_task_handle = NULL;
    }
    
    set_led_level(0);
    is_initialized = false;
    return ESP_OK;
}

esp_err_t led_controller_set_state(led_state_t state) {
    if (!is_initialized) {
        ESP_LOGE(TAG, "LED controller not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "LED state changed: %d -> %d", current_state, state);
    current_state = state;
    
    // Notify task to update LED pattern
    if (led_task_handle) {
        xTaskNotifyGive(led_task_handle);
    }
    
    return ESP_OK;
}

led_state_t led_controller_get_state(void) {
    return current_state;
}

// ===========================
// Private Functions
// ===========================

static void led_task(void *pvParameters) {
    ESP_LOGI(TAG, "LED task started");
    
    uint32_t cycle_count = 0;
    
    while (1) {
        switch (current_state) {
            case LED_STATE_OFF:
                set_led_level(0);
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // Wait for state change
                break;
                
            case LED_STATE_WIFI_CONNECTING:
                // Slow blink: 500ms on, 500ms off
                set_led_level((cycle_count % 2) ? 255 : 0);
                vTaskDelay(pdMS_TO_TICKS(LED_BLINK_SLOW_MS));
                cycle_count++;
                break;
                
            case LED_STATE_WIFI_CONNECTED:
                // Solid on
                set_led_level(255);
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // Wait for state change
                break;
                
            case LED_STATE_RECORDING:
                // Solid on (bright)
                set_led_level(255);
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // Wait for state change
                break;
                
            case LED_STATE_PLAYBACK: {
                // Pulsing effect: sine wave approximation
                uint32_t phase = (cycle_count * 50) % LED_PULSE_PERIOD_MS;
                float normalized = (float)phase / LED_PULSE_PERIOD_MS;  // 0.0 to 1.0
                float sine_approx = 0.5f + 0.5f * sinf(normalized * 2.0f * 3.14159f);
                uint8_t duty = (uint8_t)(sine_approx * 255);
                set_led_level(duty);
                vTaskDelay(pdMS_TO_TICKS(50));
                cycle_count++;
                break;
            }
                
            case LED_STATE_ERROR:
                // Fast blink: 100ms on, 100ms off
                set_led_level((cycle_count % 2) ? 255 : 0);
                vTaskDelay(pdMS_TO_TICKS(LED_BLINK_FAST_MS));
                cycle_count++;
                break;
                
            default:
                set_led_level(0);
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                break;
        }
    }
}

static void set_led_level(uint8_t level) {
    // Use PWM for smooth brightness control
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LED_PWM_CHANNEL, level);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LED_PWM_CHANNEL);
}

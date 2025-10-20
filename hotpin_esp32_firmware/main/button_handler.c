/**
 * @file button_handler.c
 * @brief Implementation of button FSM with debouncing and click pattern detection
 */

#include "button_handler.h"
#include "config.h"
#include "event_dispatcher.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"

// ===========================
// Private Constants
// ===========================
#define DEBOUNCE_DELAY_MS         50
#define DOUBLE_CLICK_WINDOW_MS    400
#define LONG_PRESS_THRESHOLD_MS   3000

// ===========================
// Private Variables
// ===========================
static const char *TAG = TAG_BUTTON;
static TaskHandle_t button_task_handle = NULL;
static TimerHandle_t debounce_timer = NULL;
static TimerHandle_t long_press_timer = NULL;
static TimerHandle_t double_click_timer = NULL;
static bool s_isr_service_installed = false;
static bool s_input_primed = false;

// FSM state
static button_state_t current_state = BUTTON_STATE_IDLE;
static uint32_t press_timestamp = 0;
static uint32_t release_timestamp = 0;
static uint32_t press_count = 0;
static uint8_t click_counter = 0;
static bool isr_triggered = false;

// ===========================
// Forward Declarations
// ===========================
static void button_isr_handler(void *arg);
static void button_fsm_task(void *pvParameters);
static void debounce_timer_callback(TimerHandle_t xTimer);
static void long_press_timer_callback(TimerHandle_t xTimer);
static void double_click_timer_callback(TimerHandle_t xTimer);
static void post_button_event(button_event_type_t event_type, uint32_t duration_ms);
static uint32_t get_millis(void);

// ===========================
// Public Functions
// ===========================

esp_err_t button_handler_init(void) {
    ESP_LOGI(TAG, "Initializing button handler on GPIO %d", CONFIG_PUSH_BUTTON_GPIO);
    
    if (event_dispatcher_queue() == NULL) {
        ESP_LOGE(TAG, "Event dispatcher not ready");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Configure GPIO as input with pull-up
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << CONFIG_PUSH_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE  // Trigger on both edges
    };
    
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure GPIO: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Create timers
    debounce_timer = xTimerCreate("debounce", pdMS_TO_TICKS(DEBOUNCE_DELAY_MS), 
                                   pdFALSE, NULL, debounce_timer_callback);
    long_press_timer = xTimerCreate("long_press", pdMS_TO_TICKS(LONG_PRESS_THRESHOLD_MS), 
                                     pdFALSE, NULL, long_press_timer_callback);
    double_click_timer = xTimerCreate("double_click", pdMS_TO_TICKS(DOUBLE_CLICK_WINDOW_MS), 
                                       pdFALSE, NULL, double_click_timer_callback);
    
    if (!debounce_timer || !long_press_timer || !double_click_timer) {
        ESP_LOGE(TAG, "Failed to create timers");
        return ESP_ERR_NO_MEM;
    }
    
    // Create button FSM task
    BaseType_t task_ret = xTaskCreatePinnedToCore(
        button_fsm_task,
        "button_fsm",
        TASK_STACK_SIZE_SMALL,
        NULL,
        TASK_PRIORITY_BUTTON_HANDLER,
        &button_task_handle,
        TASK_CORE_AUDIO_IO  // Core 0 for I/O handling
    );
    
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create button task");
        return ESP_ERR_NO_MEM;
    }
    
    // Install GPIO ISR service and add handler
    if (!s_isr_service_installed) {
        ret = gpio_install_isr_service(ESP_INTR_FLAG_LEVEL3);
        if (ret == ESP_OK) {
            s_isr_service_installed = true;
        } else if (ret == ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "GPIO ISR service already installed (shared)" );
            s_isr_service_installed = true;
        } else {
            ESP_LOGE(TAG, "Failed to install ISR service: %s", esp_err_to_name(ret));
            return ret;
        }
    }
    
    ret = gpio_isr_handler_add(CONFIG_PUSH_BUTTON_GPIO, button_isr_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ISR handler: %s", esp_err_to_name(ret));
        return ret;
    }
    
    int initial_level = gpio_get_level(CONFIG_PUSH_BUTTON_GPIO);
    s_input_primed = (initial_level != 0);
    current_state = s_input_primed ? BUTTON_STATE_IDLE : BUTTON_STATE_WAIT_RELEASE;
    if (!s_input_primed) {
        ESP_LOGW(TAG, "Button input low at init - waiting for release before enabling detection");
    }

    ESP_LOGI(TAG, "Button handler initialized successfully (initial level=%d, primed=%d)",
             initial_level, s_input_primed);
    return ESP_OK;
}

esp_err_t button_handler_deinit(void) {
    ESP_LOGI(TAG, "Deinitializing button handler");
    
    // Remove ISR handler
    gpio_isr_handler_remove(CONFIG_PUSH_BUTTON_GPIO);
    
    // Stop and delete timers
    if (debounce_timer) xTimerDelete(debounce_timer, portMAX_DELAY);
    if (long_press_timer) xTimerDelete(long_press_timer, portMAX_DELAY);
    if (double_click_timer) xTimerDelete(double_click_timer, portMAX_DELAY);
    
    // Delete task
    if (button_task_handle) {
        vTaskDelete(button_task_handle);
        button_task_handle = NULL;
    }
    
    current_state = BUTTON_STATE_IDLE;
    s_input_primed = true;
    return ESP_OK;
}

bool button_handler_isr_service_installed(void) {
    return s_isr_service_installed;
}

button_state_t button_handler_get_state(void) {
    return current_state;
}

uint32_t button_handler_get_press_count(void) {
    return press_count;
}

void button_handler_reset(void) {
    ESP_LOGW(TAG, "Resetting button FSM");
    int level = gpio_get_level(CONFIG_PUSH_BUTTON_GPIO);
    s_input_primed = (level != 0);
    current_state = s_input_primed ? BUTTON_STATE_IDLE : BUTTON_STATE_WAIT_RELEASE;
    click_counter = 0;
    isr_triggered = false;
    xTimerStop(debounce_timer, 0);
    xTimerStop(long_press_timer, 0);
    xTimerStop(double_click_timer, 0);
    if (!s_input_primed) {
        ESP_LOGW(TAG, "Button reset while held low - waiting for release to re-prime");
    }
}

// ===========================
// Private Functions
// ===========================

static void button_isr_handler(void *arg) {
    // Set flag for FSM task to handle
    isr_triggered = true;
    
    // Wake up button task
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (button_task_handle) {
        vTaskNotifyGiveFromISR(button_task_handle, &xHigherPriorityTaskWoken);
    }
    
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

static void button_fsm_task(void *pvParameters) {
    ESP_LOGI(TAG, "Button FSM task started");
    
    while (1) {
        // Wait for ISR notification
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        
        if (!isr_triggered) continue;
        isr_triggered = false;
        
        // Read current GPIO level (0 = pressed, 1 = released with pull-up)
        int gpio_level = gpio_get_level(CONFIG_PUSH_BUTTON_GPIO);
        
        // FSM logic
        switch (current_state) {
            case BUTTON_STATE_IDLE:
                if (!s_input_primed) {
                    ESP_LOGW(TAG, "Ignoring press while waiting for initial release");
                    current_state = BUTTON_STATE_WAIT_RELEASE;
                    break;
                }

                if (gpio_level == 0) {  // Button pressed
                    current_state = BUTTON_STATE_DEBOUNCE_PRESS;
                    xTimerStart(debounce_timer, 0);
                }
                break;
                
            case BUTTON_STATE_DEBOUNCE_PRESS:
                // Wait for debounce timer callback
                break;
                
            case BUTTON_STATE_PRESSED:
                if (gpio_level == 1) {  // Button released
                    current_state = BUTTON_STATE_DEBOUNCE_RELEASE;
                    xTimerStop(long_press_timer, 0);
                    xTimerStart(debounce_timer, 0);
                }
                break;
                
            case BUTTON_STATE_LONG_PRESS:
                if (gpio_level == 1) {  // Long press released
                    uint32_t duration = get_millis() - press_timestamp;
                    post_button_event(BUTTON_EVENT_LONG_PRESS_RELEASE, duration);
                    current_state = BUTTON_STATE_IDLE;
                }
                break;
                
            case BUTTON_STATE_DEBOUNCE_RELEASE:
            case BUTTON_STATE_WAIT_RELEASE:
                if (current_state == BUTTON_STATE_WAIT_RELEASE && gpio_level == 1) {
                    s_input_primed = true;
                    current_state = BUTTON_STATE_IDLE;
                    ESP_LOGI(TAG, "Button release detected - input primed");
                }
                // Wait for debounce timer or state change
                break;
                
            default:
                ESP_LOGW(TAG, "Unknown state: %d", current_state);
                current_state = BUTTON_STATE_IDLE;
                break;
        }
    }
}

static void debounce_timer_callback(TimerHandle_t xTimer) {
    int gpio_level = gpio_get_level(CONFIG_PUSH_BUTTON_GPIO);
    
    if (current_state == BUTTON_STATE_DEBOUNCE_PRESS) {
        if (gpio_level == 0) {  // Still pressed after debounce
            press_timestamp = get_millis();
            press_count++;
            current_state = BUTTON_STATE_PRESSED;
            xTimerStart(long_press_timer, 0);
            ESP_LOGD(TAG, "Button press confirmed (count: %lu)", press_count);
        } else {
            current_state = BUTTON_STATE_IDLE;  // False trigger
        }
    } 
    else if (current_state == BUTTON_STATE_DEBOUNCE_RELEASE) {
        if (gpio_level == 1) {  // Still released after debounce
            release_timestamp = get_millis();
            uint32_t press_duration = release_timestamp - press_timestamp;
            
            if (press_duration < LONG_PRESS_THRESHOLD_MS) {
                // Count as click
                click_counter++;
                ESP_LOGD(TAG, "Click registered (count: %d)", click_counter);
                
                if (click_counter == 1) {
                    // Start double-click window
                    xTimerStart(double_click_timer, 0);
                    current_state = BUTTON_STATE_IDLE;
                } else if (click_counter == 2) {
                    // Double click detected
                    xTimerStop(double_click_timer, 0);
                    post_button_event(BUTTON_EVENT_DOUBLE_CLICK, 0);
                    click_counter = 0;
                    current_state = BUTTON_STATE_IDLE;
                }
            }
        }
    }
}

static void long_press_timer_callback(TimerHandle_t xTimer) {
    if (current_state == BUTTON_STATE_PRESSED) {
        uint32_t duration = get_millis() - press_timestamp;
        post_button_event(BUTTON_EVENT_LONG_PRESS, duration);
        current_state = BUTTON_STATE_LONG_PRESS;
        ESP_LOGI(TAG, "Long press detected (%lu ms)", duration);
    }
}

static void double_click_timer_callback(TimerHandle_t xTimer) {
    // Double-click window expired, treat as single click
    if (click_counter == 1) {
        post_button_event(BUTTON_EVENT_SINGLE_CLICK, 0);
        ESP_LOGI(TAG, "Single click confirmed");
    }
    click_counter = 0;
}

static void post_button_event(button_event_type_t event_type, uint32_t duration_ms) {
    system_event_t evt = {
        .type = SYSTEM_EVENT_BUTTON_INPUT,
        .timestamp_ms = get_millis(),
        .data.button = {
            .type = event_type,
            .duration_ms = duration_ms,
        },
    };

    if (!event_dispatcher_post(&evt, 0)) {
        ESP_LOGW(TAG, "Failed to post button event (queue full)");
    }
}

static uint32_t get_millis(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

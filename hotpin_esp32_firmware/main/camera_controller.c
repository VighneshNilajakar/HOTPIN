/**
 * @file camera_controller.c
 * @brief OV2640 camera controller implementation
 */

#include "camera_controller.h"
#include "config.h"
#include "button_handler.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "driver/gpio.h"

static const char *TAG = TAG_CAMERA;
static bool is_initialized = false;
static bool flash_initialized = false;

// Initialize the flash LED on GPIO 4
static esp_err_t flash_led_init(void) {
    if (flash_initialized) {
        return ESP_OK;
    }
    
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << CONFIG_CAMERA_FLASH_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    
    esp_err_t ret = gpio_config(&io_conf);
    if (ret == ESP_OK) {
        gpio_set_level(CONFIG_CAMERA_FLASH_GPIO, 0);  // Initially off
        flash_initialized = true;
        ESP_LOGI(TAG, "Flash LED initialized on GPIO %d", CONFIG_CAMERA_FLASH_GPIO);
    } else {
        ESP_LOGE(TAG, "Failed to initialize flash LED: %s", esp_err_to_name(ret));
    }
    
    return ret;
}

// Turn flash LED on
static void flash_led_on(void) {
    if (flash_initialized) {
        gpio_set_level(CONFIG_CAMERA_FLASH_GPIO, 1);
        ESP_LOGD(TAG, "Flash LED turned ON");
    }
}

// Turn flash LED off
static void flash_led_off(void) {
    if (flash_initialized) {
        gpio_set_level(CONFIG_CAMERA_FLASH_GPIO, 0);
        ESP_LOGD(TAG, "Flash LED turned OFF");
    }
}

esp_err_t camera_controller_init(void) {
    ESP_LOGI(TAG, "Initializing camera...");
    
    // Initialize flash LED
    flash_led_init();
    
    // Camera configuration with AI-Thinker pin mapping
    camera_config_t camera_config = {
        .pin_pwdn = CONFIG_CAMERA_PIN_PWDN,
        .pin_reset = CONFIG_CAMERA_PIN_RESET,
        .pin_xclk = CONFIG_CAMERA_PIN_XCLK,
        .pin_sscb_sda = CONFIG_CAMERA_PIN_SIOD,
        .pin_sscb_scl = CONFIG_CAMERA_PIN_SIOC,
        .pin_d7 = CONFIG_CAMERA_PIN_D7,
        .pin_d6 = CONFIG_CAMERA_PIN_D6,
        .pin_d5 = CONFIG_CAMERA_PIN_D5,
        .pin_d4 = CONFIG_CAMERA_PIN_D4,
        .pin_d3 = CONFIG_CAMERA_PIN_D3,
        .pin_d2 = CONFIG_CAMERA_PIN_D2,
        .pin_d1 = CONFIG_CAMERA_PIN_D1,
        .pin_d0 = CONFIG_CAMERA_PIN_D0,
        .pin_vsync = CONFIG_CAMERA_PIN_VSYNC,
        .pin_href = CONFIG_CAMERA_PIN_HREF,
        .pin_pclk = CONFIG_CAMERA_PIN_PCLK,
        
        .xclk_freq_hz = CONFIG_CAMERA_XCLK_FREQ,
        .ledc_timer = LEDC_TIMER_2,
        .ledc_channel = LEDC_CHANNEL_2,
        
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_VGA,
        .jpeg_quality = 12,
        .fb_count = 1,  // ✅ FIX: Reduced from 2 to 1 to free ~61KB PSRAM for I2S DMA buffers
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY
    };
    
    esp_err_t ret = esp_camera_init(&camera_config);
    if (ret == ESP_OK) {
        is_initialized = true;
        ESP_LOGI(TAG, "Camera initialized successfully");
    } else {
        ESP_LOGE(TAG, "Camera init failed: %s", esp_err_to_name(ret));
    }
    
    return ret;
}

esp_err_t camera_controller_deinit(void) {
    ESP_LOGI(TAG, "Deinitializing camera...");
    
    if (!is_initialized) {
        return ESP_OK;
    }
    
    // Add hardware reset
    if (CONFIG_CAMERA_PIN_RESET != -1) {
        gpio_set_level(CONFIG_CAMERA_PIN_RESET, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(CONFIG_CAMERA_PIN_RESET, 1);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    esp_err_t ret = esp_camera_deinit();
    if (ret == ESP_OK) {
        is_initialized = false;
        ESP_LOGI(TAG, "Camera deinitialized");
    } else {
        ESP_LOGE(TAG, "Camera deinit failed: %s", esp_err_to_name(ret));
    }
    
    return ret;
}

camera_fb_t* camera_controller_capture_frame(void) {
    if (!is_initialized) {
        ESP_LOGE(TAG, "Camera not initialized");
        return NULL;
    }
    
    // Turn on flash LED before capture
    flash_led_on();
    
    // ✅ CRITICAL FIX: Extended flash duration for better illumination
    // 100ms was too short - LED needs time to reach full brightness and illuminate scene
    // 500ms provides adequate lighting for proper image exposure
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // Capture the frame
    camera_fb_t *fb = esp_camera_fb_get();
    
    // Turn off flash LED immediately after capture
    flash_led_off();
    
    return fb;
}

bool camera_controller_is_initialized(void) {
    return is_initialized;
}

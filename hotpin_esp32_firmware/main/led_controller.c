/**
 * @file led_controller.c
 * @brief Dynamic LED feedback patterns for HotPin device states.
 */

#include "led_controller.h"
#include "config.h"
#include "esp_log.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdbool.h>

#define LED_PWM_CHANNEL         LEDC_CHANNEL_1
#define LED_PWM_TIMER           LEDC_TIMER_1
#define LED_PWM_MODE            LEDC_LOW_SPEED_MODE
#define LED_PWM_DUTY_RES        LEDC_TIMER_10_BIT
#define LED_PWM_FREQUENCY       5000
#define LED_MAX_DUTY            ((1U << LED_PWM_DUTY_RES) - 1U)

#define LED_FAST_BLINK_ON_MS    100
#define LED_FAST_BLINK_OFF_MS   100
#define LED_PULSE_ON_MS         500
#define LED_PULSE_OFF_MS        500
#define LED_BREATH_FADE_MS      1500
#define LED_BREATH_PAUSE_MS     200
#define LED_FLASH_ON_MS         120
#define LED_FLASH_PAUSE_MS      100
#define LED_SOS_SHORT_MS        120
#define LED_SOS_LONG_MS         360
#define LED_SOS_GAP_MS          160
#define LED_SOS_REPEAT_PAUSE_MS 600

static const char *TAG = "LED_CTRL";

static TaskHandle_t s_led_task = NULL;
static SemaphoreHandle_t s_state_mutex = NULL;
static led_state_t s_led_state = LED_STATE_OFF;
static bool s_initialized = false;
static bool s_fade_installed = false;

static void led_task(void *pvParameters);
static void set_led_duty(uint32_t duty);
static bool wait_for_state_change(TickType_t ticks_to_wait);
static led_state_t get_led_state(void);
static void install_fade_if_needed(void);
static void start_fade(uint32_t duty, uint32_t time_ms);

esp_err_t led_controller_init(void) {
	if (s_initialized) {
		return ESP_OK;
	}

	ESP_LOGI(TAG, "Initializing LED controller on GPIO %d", CONFIG_STATUS_LED_GPIO);

	ledc_timer_config_t timer_cfg = {
		.speed_mode = LED_PWM_MODE,
		.timer_num = LED_PWM_TIMER,
		.duty_resolution = LED_PWM_DUTY_RES,
		.freq_hz = LED_PWM_FREQUENCY,
		.clk_cfg = LEDC_AUTO_CLK,
	};
	esp_err_t ret = ledc_timer_config(&timer_cfg);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to configure LEDC timer: %s", esp_err_to_name(ret));
		return ret;
	}

	ledc_channel_config_t ch_cfg = {
		.speed_mode = LED_PWM_MODE,
		.channel = LED_PWM_CHANNEL,
		.timer_sel = LED_PWM_TIMER,
		.intr_type = LEDC_INTR_DISABLE,
		.gpio_num = CONFIG_STATUS_LED_GPIO,
		.duty = 0,
		.hpoint = 0,
	};
	ret = ledc_channel_config(&ch_cfg);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to configure LEDC channel: %s", esp_err_to_name(ret));
		return ret;
	}

	install_fade_if_needed();

	if (s_state_mutex == NULL) {
		s_state_mutex = xSemaphoreCreateMutex();
		if (s_state_mutex == NULL) {
			ESP_LOGE(TAG, "Failed to create LED state mutex");
			return ESP_ERR_NO_MEM;
		}
	}

	s_led_state = LED_STATE_OFF;
	set_led_duty(0);

	BaseType_t task_ret = xTaskCreatePinnedToCore(
		led_task,
		"led_pattern",
		TASK_STACK_SIZE_SMALL,
		NULL,
		1,
		&s_led_task,
		TASK_CORE_AUDIO_IO
	);

	if (task_ret != pdPASS) {
		ESP_LOGE(TAG, "Failed to create LED task");
		vSemaphoreDelete(s_state_mutex);
		s_state_mutex = NULL;
		return ESP_ERR_NO_MEM;
	}

	s_initialized = true;
	ESP_LOGI(TAG, "LED controller ready");
	return ESP_OK;
}

esp_err_t led_controller_deinit(void) {
	if (!s_initialized) {
		return ESP_OK;
	}

	if (s_led_task != NULL) {
		vTaskDelete(s_led_task);
		s_led_task = NULL;
	}

	set_led_duty(0);

	if (s_state_mutex != NULL) {
		vSemaphoreDelete(s_state_mutex);
		s_state_mutex = NULL;
	}

	s_initialized = false;
	return ESP_OK;
}

esp_err_t led_controller_set_state(led_state_t state) {
	if (!s_initialized) {
		return ESP_ERR_INVALID_STATE;
	}

	if (state < LED_STATE_OFF || state > LED_STATE_FLASH) {
		return ESP_ERR_INVALID_ARG;
	}

	if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
		return ESP_ERR_TIMEOUT;
	}

	led_state_t previous = s_led_state;
	s_led_state = state;
	xSemaphoreGive(s_state_mutex);

	if (previous != state && s_led_task != NULL) {
		ESP_LOGI(TAG, "LED pattern -> %d", state);
		xTaskNotifyGive(s_led_task);
	}

	return ESP_OK;
}

led_state_t led_controller_get_state(void) {
	if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
		return s_led_state;
	}
	led_state_t state = s_led_state;
	xSemaphoreGive(s_state_mutex);
	return state;
}

static void led_task(void *pvParameters) {
	(void)pvParameters;

	for (;;) {
		led_state_t state = get_led_state();

		switch (state) {
			case LED_STATE_OFF:
				set_led_duty(0);
				wait_for_state_change(portMAX_DELAY);
				break;

			case LED_STATE_SOLID:
				set_led_duty(LED_MAX_DUTY);
				wait_for_state_change(portMAX_DELAY);
				break;

			case LED_STATE_FAST_BLINK:
				while (get_led_state() == LED_STATE_FAST_BLINK) {
					set_led_duty(LED_MAX_DUTY);
					if (wait_for_state_change(pdMS_TO_TICKS(LED_FAST_BLINK_ON_MS))) {
						break;
					}
					set_led_duty(0);
					if (wait_for_state_change(pdMS_TO_TICKS(LED_FAST_BLINK_OFF_MS))) {
						break;
					}
				}
				break;

			case LED_STATE_BREATHING:
				set_led_duty(0);
				while (get_led_state() == LED_STATE_BREATHING) {
					start_fade(LED_MAX_DUTY, LED_BREATH_FADE_MS);
					if (wait_for_state_change(pdMS_TO_TICKS(LED_BREATH_FADE_MS))) {
						break;
					}
					start_fade(0, LED_BREATH_FADE_MS);
					if (wait_for_state_change(pdMS_TO_TICKS(LED_BREATH_FADE_MS))) {
						break;
					}
					if (wait_for_state_change(pdMS_TO_TICKS(LED_BREATH_PAUSE_MS))) {
						break;
					}
				}
				break;

			case LED_STATE_PULSING:
				while (get_led_state() == LED_STATE_PULSING) {
					set_led_duty(LED_MAX_DUTY);
					if (wait_for_state_change(pdMS_TO_TICKS(LED_PULSE_ON_MS))) {
						break;
					}
					set_led_duty(LED_MAX_DUTY / 8U);
					if (wait_for_state_change(pdMS_TO_TICKS(LED_PULSE_OFF_MS))) {
						break;
					}
				}
				break;

			case LED_STATE_SOS: {
				bool exit_pattern = false;
				while (!exit_pattern && get_led_state() == LED_STATE_SOS) {
					for (int i = 0; i < 3 && !exit_pattern; ++i) {
						set_led_duty(LED_MAX_DUTY);
						if (wait_for_state_change(pdMS_TO_TICKS(LED_SOS_SHORT_MS))) {
							exit_pattern = true;
							break;
						}
						set_led_duty(0);
						if (wait_for_state_change(pdMS_TO_TICKS(LED_SOS_GAP_MS))) {
							exit_pattern = true;
							break;
						}
					}

					for (int i = 0; i < 3 && !exit_pattern; ++i) {
						set_led_duty(LED_MAX_DUTY);
						if (wait_for_state_change(pdMS_TO_TICKS(LED_SOS_LONG_MS))) {
							exit_pattern = true;
							break;
						}
						set_led_duty(0);
						if (wait_for_state_change(pdMS_TO_TICKS(LED_SOS_GAP_MS))) {
							exit_pattern = true;
							break;
						}
					}

					for (int i = 0; i < 3 && !exit_pattern; ++i) {
						set_led_duty(LED_MAX_DUTY);
						if (wait_for_state_change(pdMS_TO_TICKS(LED_SOS_SHORT_MS))) {
							exit_pattern = true;
							break;
						}
						set_led_duty(0);
						if (wait_for_state_change(pdMS_TO_TICKS(LED_SOS_GAP_MS))) {
							exit_pattern = true;
							break;
						}
					}

					if (!exit_pattern) {
						if (wait_for_state_change(pdMS_TO_TICKS(LED_SOS_REPEAT_PAUSE_MS))) {
							exit_pattern = true;
						}
					}
				}
				set_led_duty(0);
				break;
			}

			case LED_STATE_FLASH:
				set_led_duty(LED_MAX_DUTY);
				if (wait_for_state_change(pdMS_TO_TICKS(LED_FLASH_ON_MS))) {
					break;
				}
				set_led_duty(0);
				if (wait_for_state_change(pdMS_TO_TICKS(LED_FLASH_PAUSE_MS))) {
					break;
				}

				if (get_led_state() == LED_STATE_FLASH) {
					if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
						s_led_state = LED_STATE_OFF;
						xSemaphoreGive(s_state_mutex);
					}
				}
				break;
		}
	}
}

static void set_led_duty(uint32_t duty) {
	if (duty > LED_MAX_DUTY) {
		duty = LED_MAX_DUTY;
	}
	ledc_set_duty(LED_PWM_MODE, LED_PWM_CHANNEL, duty);
	ledc_update_duty(LED_PWM_MODE, LED_PWM_CHANNEL);
}

static bool wait_for_state_change(TickType_t ticks_to_wait) {
	return ulTaskNotifyTake(pdTRUE, ticks_to_wait) > 0;
}

static led_state_t get_led_state(void) {
	if (s_state_mutex == NULL) {
		return s_led_state;
	}
	if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
		return s_led_state;
	}
	led_state_t state = s_led_state;
	xSemaphoreGive(s_state_mutex);
	return state;
}

static void install_fade_if_needed(void) {
	if (!s_fade_installed) {
		esp_err_t ret = ledc_fade_func_install(0);
		if (ret == ESP_OK) {
			s_fade_installed = true;
		} else {
			ESP_LOGW(TAG, "LEDC fade install failed: %s", esp_err_to_name(ret));
		}
	}
}

static void start_fade(uint32_t duty, uint32_t time_ms) {
	if (!s_fade_installed) {
		set_led_duty(duty);
		return;
	}

	if (duty > LED_MAX_DUTY) {
		duty = LED_MAX_DUTY;
	}

	ledc_set_fade_with_time(LED_PWM_MODE, LED_PWM_CHANNEL, duty, time_ms);
	ledc_fade_start(LED_PWM_MODE, LED_PWM_CHANNEL, LEDC_FADE_NO_WAIT);
}

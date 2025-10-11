#include "event_dispatcher.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define EVENT_QUEUE_DEPTH 16

static const char *TAG = "event_dispatcher";
static QueueHandle_t s_event_queue = NULL;

void event_dispatcher_init(void)
{
    if (s_event_queue != NULL) {
        return; // already initialized
    }

    s_event_queue = xQueueCreate(EVENT_QUEUE_DEPTH, sizeof(system_event_t));
    if (s_event_queue == NULL) {
        ESP_LOGE(TAG, "Failed to allocate system event queue");
    } else {
        ESP_LOGI(TAG, "System event queue ready (%d entries)", EVENT_QUEUE_DEPTH);
    }
}

QueueHandle_t event_dispatcher_queue(void)
{
    return s_event_queue;
}

bool event_dispatcher_post(const system_event_t *evt, TickType_t timeout_ticks)
{
    if ((evt == NULL) || (s_event_queue == NULL)) {
        return false;
    }

    if (xQueueSend(s_event_queue, evt, timeout_ticks) != pdPASS) {
        ESP_LOGW(TAG, "Queue full dropping event %d", evt->type);
        return false;
    }

    return true;
}

/**
 * @file event_dispatcher.h
 * @brief Central event queue interface bridging producers to the FSM.
 */

#ifndef EVENT_DISPATCHER_H
#define EVENT_DISPATCHER_H

#include "system_events.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the global event dispatcher and backing queue.
 *
 * This must be called before any producer attempts to post events.
 */
void event_dispatcher_init(void);

/**
 * @brief Retrieve the queue handle used by the dispatcher.
 */
QueueHandle_t event_dispatcher_queue(void);

/**
 * @brief Enqueue an event for asynchronous processing.
 *
 * @param evt Event instance to push. Callers should zero unused fields.
 * @param timeout_ticks FreeRTOS ticks to wait if the queue is full.
 * @return true on success, false if the queue overflowed or dispatcher invalid.
 */
bool event_dispatcher_post(const system_event_t *evt, TickType_t timeout_ticks);

#ifdef __cplusplus
}
#endif

#endif // EVENT_DISPATCHER_H

/** @file Queue API declarations for the MicroTech simulator host port. */
#ifndef __SIM_FREERTOS_QUEUE_H__
#define __SIM_FREERTOS_QUEUE_H__

#include <stddef.h>

#include "freertos/FreeRTOS.h"

/** @brief Create a host queue. */
QueueHandle_t xQueueCreate(uint32_t length, uint32_t item_size);
/** @brief Create a host queue inside caller-provided storage. */
QueueHandle_t xQueueCreateStatic(uint32_t length, uint32_t item_size,
                                 uint8_t *storage,
                                 StaticQueue_t *queue_storage);
/** @brief Send one item to a host queue. */
BaseType_t xQueueSend(QueueHandle_t queue, const void *item,
                      TickType_t timeout);
/** @brief Alias of xQueueSend matching the FreeRTOS name. */
#define xQueueSendToBack xQueueSend
/** @brief Send one item to the front of a host queue. */
BaseType_t xQueueSendToFront(QueueHandle_t queue, const void *item,
                             TickType_t timeout);
/** @brief ISR-shaped send (no interrupts on the host; behaves plain). */
BaseType_t xQueueSendFromISR(QueueHandle_t queue, const void *item,
                             BaseType_t *woken);
/** @brief Receive one item from a host queue. */
BaseType_t xQueueReceive(QueueHandle_t queue, void *item,
                         TickType_t timeout);
/** @brief Return the number of queued items. */
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue);
/** @brief Delete a host queue. */
void vQueueDelete(QueueHandle_t queue);

#endif /* __SIM_FREERTOS_QUEUE_H__ */

/** @file Queue API declarations for integration host tests. */
#ifndef __CROSS_LAYER_FREERTOS_QUEUE_H__
#define __CROSS_LAYER_FREERTOS_QUEUE_H__

#include <stddef.h>
#include "freertos/FreeRTOS.h"

/** @brief Create a host queue. */
QueueHandle_t xQueueCreate(uint32_t length, uint32_t item_size);
/** @brief Send one item to a host queue. */
BaseType_t xQueueSend(QueueHandle_t queue, const void *item,
                      TickType_t timeout);
/** @brief Receive one item from a host queue. */
BaseType_t xQueueReceive(QueueHandle_t queue, void *item,
                         TickType_t timeout);
/** @brief Delete a host queue. */
void vQueueDelete(QueueHandle_t queue);

#endif /* __CROSS_LAYER_FREERTOS_QUEUE_H__ */

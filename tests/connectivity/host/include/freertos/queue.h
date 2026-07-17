/** @file Queue API declarations for the connectivity host fixture. */
#ifndef __CONNECTIVITY_HOST_FREERTOS_QUEUE_H__
#define __CONNECTIVITY_HOST_FREERTOS_QUEUE_H__

#include "freertos/FreeRTOS.h"

/** @brief Create a dynamic host queue. */
QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size);
/** @brief Create a queue in caller-provided storage. */
QueueHandle_t xQueueCreateStatic(UBaseType_t length, UBaseType_t item_size,
                                 uint8_t *buffer,
                                 StaticQueue_t *queue_storage);
/** @brief Send one item to a host queue. */
BaseType_t xQueueSend(QueueHandle_t queue, const void *item,
                      TickType_t timeout_ticks);
/** @brief Receive one item from a host queue. */
BaseType_t xQueueReceive(QueueHandle_t queue, void *item,
                         TickType_t timeout_ticks);
/** @brief Delete a host queue and release owned storage. */
void vQueueDelete(QueueHandle_t queue);

#endif /* __CONNECTIVITY_HOST_FREERTOS_QUEUE_H__ */

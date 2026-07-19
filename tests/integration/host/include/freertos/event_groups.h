/** @file Event-group API declarations for integration host tests. */
#ifndef __CROSS_LAYER_FREERTOS_EVENT_GROUPS_H__
#define __CROSS_LAYER_FREERTOS_EVENT_GROUPS_H__

#include "freertos/FreeRTOS.h"

/** @brief Create a pthread-backed host event group. */
EventGroupHandle_t xEventGroupCreate(void);
/** @brief Delete a host event group. */
void vEventGroupDelete(EventGroupHandle_t group);
/** @brief Set bits and wake matching host waiters. */
EventBits_t xEventGroupSetBits(EventGroupHandle_t group,
                               EventBits_t bits);
/** @brief Clear selected host event bits. */
EventBits_t xEventGroupClearBits(EventGroupHandle_t group,
                                 EventBits_t bits);
/** @brief Read the current host event bits. */
EventBits_t xEventGroupGetBits(EventGroupHandle_t group);
/** @brief Wait for any or all selected host event bits. */
EventBits_t xEventGroupWaitBits(EventGroupHandle_t group,
                                EventBits_t bits,
                                BaseType_t clear_on_exit,
                                BaseType_t wait_for_all,
                                TickType_t timeout);

#endif /* __CROSS_LAYER_FREERTOS_EVENT_GROUPS_H__ */

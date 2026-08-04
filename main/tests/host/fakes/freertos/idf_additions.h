#ifndef __MAIN_HOST_FREERTOS_IDF_ADDITIONS_H__
#define __MAIN_HOST_FREERTOS_IDF_ADDITIONS_H__

#include "freertos/FreeRTOS.h"

BaseType_t xTaskCreatePinnedToCoreWithCaps(
    TaskFunction_t entry, const char *name, uint32_t stack_depth, void *arg,
    UBaseType_t priority, TaskHandle_t *task, BaseType_t core_id,
    UBaseType_t memory_caps);
void vTaskDeleteWithCaps(TaskHandle_t task);

#endif /* __MAIN_HOST_FREERTOS_IDF_ADDITIONS_H__ */

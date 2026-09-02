/** @file Task API declarations for the MicroTech simulator host port. */
#ifndef __SIM_FREERTOS_TASK_H__
#define __SIM_FREERTOS_TASK_H__

#include "freertos/FreeRTOS.h"

/** @brief Notification update actions modeled by the host port. */
typedef enum
{
    eNoAction = 0,
    eSetBits,
    eIncrement,
    eSetValueWithOverwrite,
    eSetValueWithoutOverwrite,
} eNotifyAction;

/** @brief Task lifecycle states reported by the host port. */
typedef enum
{
    eRunning = 0,
    eReady,
    eBlocked,
    eSuspended,
    eDeleted,
    eInvalid,
} eTaskState;

/** @brief Return the current host task handle. */
TaskHandle_t xTaskGetCurrentTaskHandle(void);
/** @brief Create a pthread-backed static task. */
TaskHandle_t xTaskCreateStaticPinnedToCore(
    void (*entry)(void *), const char *name, uint32_t stack_depth,
    void *context, UBaseType_t priority, StackType_t *stack,
    StaticTask_t *task_storage, BaseType_t core_id);
/** @brief Create a dynamically allocated pthread-backed task. */
BaseType_t xTaskCreatePinnedToCore(
    void (*entry)(void *), const char *name, uint32_t stack_depth,
    void *context, UBaseType_t priority, TaskHandle_t *out_task,
    BaseType_t core_id);
/** @brief Create a dynamically allocated task without affinity. */
BaseType_t xTaskCreate(void (*entry)(void *), const char *name,
                       uint32_t stack_depth, void *context,
                       UBaseType_t priority, TaskHandle_t *out_task);
/** @brief Return the modeled task core affinity. */
BaseType_t xTaskGetCoreID(TaskHandle_t task);
/** @brief Look up a created task by name. */
TaskHandle_t xTaskGetHandle(const char *name);
/** @brief Send a notification to a host task. */
BaseType_t xTaskNotify(TaskHandle_t task, uint32_t value,
                       eNotifyAction action);
/** @brief Wait for a host task notification. */
BaseType_t xTaskNotifyWait(uint32_t clear_on_entry, uint32_t clear_on_exit,
                           uint32_t *value, TickType_t timeout_ticks);
/** @brief Increment a host task's notification count. */
BaseType_t xTaskNotifyGive(TaskHandle_t task);
/** @brief Wait for and consume the current task's notification count. */
uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit,
                          TickType_t timeout_ticks);
/** @brief Return monotonic host ticks. */
TickType_t xTaskGetTickCount(void);
/** @brief Delay the current task for a number of host ticks. */
void vTaskDelay(TickType_t ticks);
/** @brief Delete the current task or a dynamically allocated task. */
void vTaskDelete(TaskHandle_t task);
/** @brief Suspend a task (modeled: blocks on next delay point). */
void vTaskSuspend(TaskHandle_t task);
/** @brief Resume a task suspended by vTaskSuspend(). */
void vTaskResume(TaskHandle_t task);
/** @brief Change a task priority (recorded; pthreads keep equal weight). */
void vTaskPrioritySet(TaskHandle_t task, UBaseType_t priority);
/** @brief Report the modeled task state. */
eTaskState eTaskGetState(TaskHandle_t task);
/** @brief Return a conservative static high-water estimate. */
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);
/** @brief Return the recorded stack start (unused on the host). */
uint8_t *xTaskGetStackStart(TaskHandle_t task);

#endif /* __SIM_FREERTOS_TASK_H__ */

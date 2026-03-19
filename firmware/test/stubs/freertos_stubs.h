#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
typedef uint32_t configSTACK_DEPTH_TYPE;
typedef void (*TaskFunction_t)(void *arg);
typedef struct freertos_stub_task *TaskHandle_t;
typedef struct freertos_stub_event_group *EventGroupHandle_t;
typedef uint32_t EventBits_t;

typedef struct {
    uintptr_t opaque;
} StaticSemaphore_t;

typedef void *SemaphoreHandle_t;

#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY ((TickType_t)UINT32_MAX)
#define pdMS_TO_TICKS(xTimeInMs) ((TickType_t)(xTimeInMs))
#define BIT0 (1U << 0)
#define BIT1 (1U << 1)
#define portYIELD_FROM_ISR(xHigherPriorityTaskWoken) \
    do {                                             \
        (void)(xHigherPriorityTaskWoken);            \
    } while (0)

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buffer);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks_to_wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);
void vSemaphoreDelete(SemaphoreHandle_t semaphore);

void freertos_stub_reset(void);
void freertos_stub_set_task_create_result(BaseType_t result);
size_t freertos_stub_get_task_create_count(void);
const char *freertos_stub_get_last_task_name(void);
TaskHandle_t freertos_stub_get_last_task_handle(void);
EventGroupHandle_t xEventGroupCreate(void);
void vEventGroupDelete(EventGroupHandle_t event_group);
EventBits_t xEventGroupSetBits(EventGroupHandle_t event_group, EventBits_t bits_to_set);
EventBits_t xEventGroupClearBits(EventGroupHandle_t event_group, EventBits_t bits_to_clear);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t event_group,
                                EventBits_t bits_to_wait_for,
                                BaseType_t clear_on_exit,
                                BaseType_t wait_for_all_bits,
                                TickType_t ticks_to_wait);

BaseType_t xTaskCreate(TaskFunction_t task_code,
                       const char *name,
                       configSTACK_DEPTH_TYPE stack_depth,
                       void *parameters,
                       UBaseType_t priority,
                       TaskHandle_t *created_task);
void vTaskDelete(TaskHandle_t task_to_delete);
void vTaskDelay(TickType_t ticks_to_delay);
void vTaskGenericNotifyGiveFromISR(TaskHandle_t task_to_notify,
                                   UBaseType_t index_to_notify,
                                   BaseType_t *higher_priority_task_woken);
uint32_t ulTaskGenericNotifyTake(UBaseType_t index_to_wait_on,
                                 BaseType_t clear_count_on_exit,
                                 TickType_t ticks_to_wait);

#define vTaskNotifyGiveFromISR(xTaskToNotify, pxHigherPriorityTaskWoken) \
    vTaskGenericNotifyGiveFromISR((xTaskToNotify), 0U, (pxHigherPriorityTaskWoken))
#define ulTaskNotifyTake(xClearCountOnExit, xTicksToWait) \
    ulTaskGenericNotifyTake(0U, (xClearCountOnExit), (xTicksToWait))

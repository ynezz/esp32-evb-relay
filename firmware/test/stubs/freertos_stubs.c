#include "freertos_stubs.h"

#include <stddef.h>
#include <string.h>

struct freertos_stub_task {
    TaskFunction_t task_code;
    void *parameters;
    UBaseType_t priority;
    configSTACK_DEPTH_TYPE stack_depth;
    uint32_t notify_count;
};

struct freertos_stub_event_group {
    EventBits_t bits;
};

static struct freertos_stub_task s_task;
static struct freertos_stub_event_group s_event_group;
static BaseType_t s_task_create_result = pdPASS;
static size_t s_task_create_count;
static char s_last_task_name[32];
static int s_dynamic_mutex_token;

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buffer)
{
    return (buffer != NULL) ? buffer : NULL;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return &s_dynamic_mutex_token;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks_to_wait)
{
    (void)ticks_to_wait;
    return (semaphore != NULL) ? pdTRUE : pdFALSE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    return (semaphore != NULL) ? pdTRUE : pdFALSE;
}

void vSemaphoreDelete(SemaphoreHandle_t semaphore)
{
    (void)semaphore;
}

void freertos_stub_reset(void)
{
    memset(&s_task, 0, sizeof(s_task));
    memset(&s_event_group, 0, sizeof(s_event_group));
    s_task_create_result = pdPASS;
    s_task_create_count = 0U;
    memset(s_last_task_name, 0, sizeof(s_last_task_name));
}

void freertos_stub_set_task_create_result(BaseType_t result)
{
    s_task_create_result = result;
}

size_t freertos_stub_get_task_create_count(void)
{
    return s_task_create_count;
}

const char *freertos_stub_get_last_task_name(void)
{
    return s_last_task_name;
}

TaskHandle_t freertos_stub_get_last_task_handle(void)
{
    return (s_task_create_count > 0U) ? &s_task : NULL;
}

EventGroupHandle_t xEventGroupCreate(void)
{
    s_event_group.bits = 0U;
    return &s_event_group;
}

void vEventGroupDelete(EventGroupHandle_t event_group)
{
    if (event_group != NULL) {
        event_group->bits = 0U;
    }
}

EventBits_t xEventGroupSetBits(EventGroupHandle_t event_group, EventBits_t bits_to_set)
{
    if (event_group == NULL) {
        return 0U;
    }

    event_group->bits |= bits_to_set;
    return event_group->bits;
}

EventBits_t xEventGroupClearBits(EventGroupHandle_t event_group, EventBits_t bits_to_clear)
{
    if (event_group == NULL) {
        return 0U;
    }

    event_group->bits &= ~bits_to_clear;
    return event_group->bits;
}

EventBits_t xEventGroupWaitBits(EventGroupHandle_t event_group,
                                EventBits_t bits_to_wait_for,
                                BaseType_t clear_on_exit,
                                BaseType_t wait_for_all_bits,
                                TickType_t ticks_to_wait)
{
    EventBits_t matched_bits;

    (void)ticks_to_wait;

    if (event_group == NULL) {
        return 0U;
    }

    matched_bits = event_group->bits & bits_to_wait_for;
    if (wait_for_all_bits && (matched_bits != bits_to_wait_for)) {
        matched_bits = 0U;
    }
    if (clear_on_exit && (matched_bits != 0U)) {
        event_group->bits &= ~matched_bits;
    }

    return matched_bits;
}

BaseType_t xTaskCreate(TaskFunction_t task_code,
                       const char *name,
                       configSTACK_DEPTH_TYPE stack_depth,
                       void *parameters,
                       UBaseType_t priority,
                       TaskHandle_t *created_task)
{
    if ((task_code == NULL) || (created_task == NULL)) {
        return pdFALSE;
    }

    if (s_task_create_result != pdPASS) {
        *created_task = NULL;
        return s_task_create_result;
    }

    s_task.task_code = task_code;
    s_task.parameters = parameters;
    s_task.priority = priority;
    s_task.stack_depth = stack_depth;
    s_task.notify_count = 0U;
    ++s_task_create_count;

    if (name != NULL) {
        strncpy(s_last_task_name, name, sizeof(s_last_task_name) - 1U);
        s_last_task_name[sizeof(s_last_task_name) - 1U] = '\0';
    }

    *created_task = &s_task;
    return pdPASS;
}

void vTaskDelete(TaskHandle_t task_to_delete)
{
    (void)task_to_delete;
}

void vTaskDelay(TickType_t ticks_to_delay)
{
    (void)ticks_to_delay;
}

void vTaskGenericNotifyGiveFromISR(TaskHandle_t task_to_notify,
                                   UBaseType_t index_to_notify,
                                   BaseType_t *higher_priority_task_woken)
{
    (void)index_to_notify;

    if (task_to_notify != NULL) {
        ++task_to_notify->notify_count;
    }
    if (higher_priority_task_woken != NULL) {
        *higher_priority_task_woken = pdFALSE;
    }
}

uint32_t ulTaskGenericNotifyTake(UBaseType_t index_to_wait_on,
                                 BaseType_t clear_count_on_exit,
                                 TickType_t ticks_to_wait)
{
    uint32_t notify_count;

    (void)index_to_wait_on;
    (void)ticks_to_wait;

    notify_count = s_task.notify_count;
    if ((notify_count > 0U) && clear_count_on_exit) {
        s_task.notify_count = 0U;
    } else if (notify_count > 0U) {
        --s_task.notify_count;
    }

    return notify_count;
}

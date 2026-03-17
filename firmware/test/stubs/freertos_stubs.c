#include "freertos_stubs.h"

#include <stddef.h>

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buffer)
{
    return (buffer != NULL) ? buffer : NULL;
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

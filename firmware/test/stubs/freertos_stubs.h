#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef int BaseType_t;
typedef uint32_t TickType_t;

typedef struct {
    uintptr_t opaque;
} StaticSemaphore_t;

typedef void *SemaphoreHandle_t;

#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY ((TickType_t)UINT32_MAX)

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buffer);
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks_to_wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);

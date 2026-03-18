#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "freertos/task.h"

void esp_task_wdt_stub_reset(void);
void esp_task_wdt_stub_set_add_result(esp_err_t result);
void esp_task_wdt_stub_set_reset_result(esp_err_t result);
void esp_task_wdt_stub_set_delete_result(esp_err_t result);
void esp_task_wdt_stub_set_status_result(esp_err_t result);
size_t esp_task_wdt_stub_get_add_count(void);
size_t esp_task_wdt_stub_get_reset_count(void);
size_t esp_task_wdt_stub_get_delete_count(void);
size_t esp_task_wdt_stub_get_status_count(void);
TaskHandle_t esp_task_wdt_stub_get_last_add_task(void);
TaskHandle_t esp_task_wdt_stub_get_last_delete_task(void);
TaskHandle_t esp_task_wdt_stub_get_last_status_task(void);

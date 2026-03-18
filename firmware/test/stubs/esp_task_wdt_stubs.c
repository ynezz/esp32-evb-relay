#include "esp_task_wdt_stubs.h"

typedef struct {
    esp_err_t add_result;
    esp_err_t reset_result;
    esp_err_t delete_result;
    esp_err_t status_result;
    size_t add_count;
    size_t reset_count;
    size_t delete_count;
    size_t status_count;
    TaskHandle_t last_add_task;
    TaskHandle_t last_delete_task;
    TaskHandle_t last_status_task;
} esp_task_wdt_stub_state_t;

static esp_task_wdt_stub_state_t s_state;

void esp_task_wdt_stub_reset(void)
{
    s_state.add_result = ESP_OK;
    s_state.reset_result = ESP_OK;
    s_state.delete_result = ESP_OK;
    s_state.status_result = ESP_ERR_NOT_FOUND;
    s_state.add_count = 0U;
    s_state.reset_count = 0U;
    s_state.delete_count = 0U;
    s_state.status_count = 0U;
    s_state.last_add_task = NULL;
    s_state.last_delete_task = NULL;
    s_state.last_status_task = NULL;
}

void esp_task_wdt_stub_set_add_result(esp_err_t result)
{
    s_state.add_result = result;
}

void esp_task_wdt_stub_set_reset_result(esp_err_t result)
{
    s_state.reset_result = result;
}

void esp_task_wdt_stub_set_delete_result(esp_err_t result)
{
    s_state.delete_result = result;
}

void esp_task_wdt_stub_set_status_result(esp_err_t result)
{
    s_state.status_result = result;
}

size_t esp_task_wdt_stub_get_add_count(void)
{
    return s_state.add_count;
}

size_t esp_task_wdt_stub_get_reset_count(void)
{
    return s_state.reset_count;
}

size_t esp_task_wdt_stub_get_delete_count(void)
{
    return s_state.delete_count;
}

size_t esp_task_wdt_stub_get_status_count(void)
{
    return s_state.status_count;
}

TaskHandle_t esp_task_wdt_stub_get_last_add_task(void)
{
    return s_state.last_add_task;
}

TaskHandle_t esp_task_wdt_stub_get_last_delete_task(void)
{
    return s_state.last_delete_task;
}

TaskHandle_t esp_task_wdt_stub_get_last_status_task(void)
{
    return s_state.last_status_task;
}

esp_err_t esp_task_wdt_add(TaskHandle_t task_handle)
{
    ++s_state.add_count;
    s_state.last_add_task = task_handle;
    return s_state.add_result;
}

esp_err_t esp_task_wdt_reset(void)
{
    ++s_state.reset_count;
    return s_state.reset_result;
}

esp_err_t esp_task_wdt_delete(TaskHandle_t task_handle)
{
    ++s_state.delete_count;
    s_state.last_delete_task = task_handle;
    return s_state.delete_result;
}

esp_err_t esp_task_wdt_status(TaskHandle_t task_handle)
{
    ++s_state.status_count;
    s_state.last_status_task = task_handle;
    return s_state.status_result;
}

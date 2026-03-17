#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef const char *esp_event_base_t;

#define ESP_EVENT_DECLARE_BASE(id) extern esp_event_base_t id
#define ESP_EVENT_DEFINE_BASE(id) esp_event_base_t id = #id

void esp_event_stub_reset(void);
void esp_event_stub_set_post_result(esp_err_t result);
esp_event_base_t esp_event_stub_get_last_base(void);
int32_t esp_event_stub_get_last_id(void);
size_t esp_event_stub_copy_last_data(void *buffer, size_t buffer_size);

esp_err_t esp_event_post(esp_event_base_t event_base,
                         int32_t event_id,
                         const void *event_data,
                         size_t event_data_size,
                         uint32_t ticks_to_wait);

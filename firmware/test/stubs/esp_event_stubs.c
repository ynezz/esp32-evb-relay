#include "esp_event_stubs.h"

#include <string.h>

#include "relay_events_stub.h"

#define ESP_EVENT_STUB_MAX_DATA_SIZE 128

ESP_EVENT_DEFINE_BASE(EVB_RELAY_EVENT);

static esp_err_t s_post_result = ESP_OK;
static esp_event_base_t s_last_base;
static int32_t s_last_id;
static uint8_t s_last_data[ESP_EVENT_STUB_MAX_DATA_SIZE];
static size_t s_last_data_size;

void esp_event_stub_reset(void)
{
    s_post_result = ESP_OK;
    s_last_base = NULL;
    s_last_id = 0;
    s_last_data_size = 0U;
    memset(s_last_data, 0, sizeof(s_last_data));
}

void esp_event_stub_set_post_result(esp_err_t result)
{
    s_post_result = result;
}

esp_event_base_t esp_event_stub_get_last_base(void)
{
    return s_last_base;
}

int32_t esp_event_stub_get_last_id(void)
{
    return s_last_id;
}

size_t esp_event_stub_copy_last_data(void *buffer, size_t buffer_size)
{
    size_t bytes_to_copy = (buffer_size < s_last_data_size) ? buffer_size : s_last_data_size;

    if ((buffer != NULL) && (bytes_to_copy > 0U)) {
        memcpy(buffer, s_last_data, bytes_to_copy);
    }

    return s_last_data_size;
}

esp_err_t esp_event_post(esp_event_base_t event_base,
                         int32_t event_id,
                         const void *event_data,
                         size_t event_data_size,
                         uint32_t ticks_to_wait)
{
    size_t bytes_to_copy;

    (void)ticks_to_wait;

    s_last_base = event_base;
    s_last_id = event_id;
    s_last_data_size = event_data_size;

    bytes_to_copy = (event_data_size < sizeof(s_last_data)) ? event_data_size : sizeof(s_last_data);
    if ((event_data != NULL) && (bytes_to_copy > 0U)) {
        memcpy(s_last_data, event_data, bytes_to_copy);
    }
    if (bytes_to_copy < sizeof(s_last_data)) {
        memset(s_last_data + bytes_to_copy, 0, sizeof(s_last_data) - bytes_to_copy);
    }

    return s_post_result;
}

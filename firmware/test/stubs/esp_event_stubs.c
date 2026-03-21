#include "esp_event_stubs.h"

#include <stdbool.h>
#include <string.h>

#include "relay_events_stub.h"

#define ESP_EVENT_STUB_MAX_DATA_SIZE 128
#define ESP_EVENT_STUB_MAX_HANDLERS 16

ESP_EVENT_DEFINE_BASE(EVB_RELAY_EVENT);
ESP_EVENT_DEFINE_BASE(ETH_EVENT);
ESP_EVENT_DEFINE_BASE(IP_EVENT);
ESP_EVENT_DEFINE_BASE(WIFI_EVENT);

typedef struct {
    bool active;
    esp_event_base_t base;
    int32_t id;
    esp_event_handler_t handler;
} esp_event_stub_handler_t;

static esp_err_t s_post_result = ESP_OK;
static esp_err_t s_handler_register_result = ESP_OK;
static esp_event_base_t s_last_base;
static int32_t s_last_id;
static uint8_t s_last_data[ESP_EVENT_STUB_MAX_DATA_SIZE];
static size_t s_last_data_size;
static size_t s_post_count;
static esp_event_stub_handler_t s_handlers[ESP_EVENT_STUB_MAX_HANDLERS];
static size_t s_handler_register_count;
static size_t s_handler_unregister_count;

void esp_event_stub_reset(void)
{
    s_post_result = ESP_OK;
    s_handler_register_result = ESP_OK;
    s_last_base = NULL;
    s_last_id = 0;
    s_last_data_size = 0U;
    s_post_count = 0U;
    memset(s_last_data, 0, sizeof(s_last_data));
    memset(s_handlers, 0, sizeof(s_handlers));
    s_handler_register_count = 0U;
    s_handler_unregister_count = 0U;
}

void esp_event_stub_set_post_result(esp_err_t result)
{
    s_post_result = result;
}

void esp_event_stub_set_handler_register_result(esp_err_t result)
{
    s_handler_register_result = result;
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

size_t esp_event_stub_get_post_count(void)
{
    return s_post_count;
}

size_t esp_event_stub_get_handler_register_count(void)
{
    return s_handler_register_count;
}

size_t esp_event_stub_get_handler_unregister_count(void)
{
    return s_handler_unregister_count;
}

size_t esp_event_stub_get_active_handler_count(void)
{
    size_t active_count = 0U;

    for (size_t index = 0; index < ESP_EVENT_STUB_MAX_HANDLERS; ++index) {
        if (s_handlers[index].active) {
            ++active_count;
        }
    }

    return active_count;
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
    ++s_post_count;

    bytes_to_copy = (event_data_size < sizeof(s_last_data)) ? event_data_size : sizeof(s_last_data);
    if ((event_data != NULL) && (bytes_to_copy > 0U)) {
        memcpy(s_last_data, event_data, bytes_to_copy);
    }
    if (bytes_to_copy < sizeof(s_last_data)) {
        memset(s_last_data + bytes_to_copy, 0, sizeof(s_last_data) - bytes_to_copy);
    }

    return s_post_result;
}

esp_err_t esp_event_handler_register(esp_event_base_t event_base,
                                     int32_t event_id,
                                     esp_event_handler_t event_handler,
                                     void *event_handler_arg)
{
    (void)event_handler_arg;

    if (s_handler_register_result != ESP_OK) {
        return s_handler_register_result;
    }

    for (size_t index = 0; index < ESP_EVENT_STUB_MAX_HANDLERS; ++index) {
        if (s_handlers[index].active) {
            continue;
        }

        s_handlers[index].active = true;
        s_handlers[index].base = event_base;
        s_handlers[index].id = event_id;
        s_handlers[index].handler = event_handler;
        ++s_handler_register_count;
        return ESP_OK;
    }

    return ESP_ERR_NO_MEM;
}

esp_err_t esp_event_handler_unregister(esp_event_base_t event_base,
                                       int32_t event_id,
                                       esp_event_handler_t event_handler)
{
    for (size_t index = 0; index < ESP_EVENT_STUB_MAX_HANDLERS; ++index) {
        if (!s_handlers[index].active) {
            continue;
        }
        if ((s_handlers[index].base != event_base) ||
                (s_handlers[index].id != event_id) ||
                (s_handlers[index].handler != event_handler)) {
            continue;
        }

        s_handlers[index].active = false;
        s_handlers[index].base = NULL;
        s_handlers[index].id = 0;
        s_handlers[index].handler = NULL;
        ++s_handler_unregister_count;
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

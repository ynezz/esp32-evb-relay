#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef const char *esp_event_base_t;
typedef void (*esp_event_handler_t)(void *arg,
                                    esp_event_base_t event_base,
                                    int32_t event_id,
                                    void *event_data);

#define ESP_EVENT_DECLARE_BASE(id) extern esp_event_base_t id
#define ESP_EVENT_DEFINE_BASE(id) esp_event_base_t id = #id
#define ESP_EVENT_ANY_ID (-1)

#define IP_EVENT_ETH_GOT_IP 0x100
#define IP_EVENT_STA_GOT_IP 0x101

ESP_EVENT_DECLARE_BASE(EVB_RELAY_EVENT);
ESP_EVENT_DECLARE_BASE(ETH_EVENT);
ESP_EVENT_DECLARE_BASE(IP_EVENT);
ESP_EVENT_DECLARE_BASE(WIFI_EVENT);

void esp_event_stub_reset(void);
void esp_event_stub_set_post_result(esp_err_t result);
void esp_event_stub_set_handler_register_result(esp_err_t result);
esp_event_base_t esp_event_stub_get_last_base(void);
int32_t esp_event_stub_get_last_id(void);
size_t esp_event_stub_copy_last_data(void *buffer, size_t buffer_size);
size_t esp_event_stub_get_handler_register_count(void);
size_t esp_event_stub_get_handler_unregister_count(void);
size_t esp_event_stub_get_active_handler_count(void);

esp_err_t esp_event_post(esp_event_base_t event_base,
                         int32_t event_id,
                         const void *event_data,
                         size_t event_data_size,
                         uint32_t ticks_to_wait);
esp_err_t esp_event_handler_register(esp_event_base_t event_base,
                                     int32_t event_id,
                                     esp_event_handler_t event_handler,
                                     void *event_handler_arg);
esp_err_t esp_event_handler_unregister(esp_event_base_t event_base,
                                       int32_t event_id,
                                       esp_event_handler_t event_handler);

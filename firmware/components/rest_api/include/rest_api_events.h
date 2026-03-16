#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(EVB_RELAY_EVENT);

typedef enum {
    EVB_RELAY_EVENT_DIGITAL_INPUT = 1,
    EVB_RELAY_EVENT_ANALOG_INPUT,
    EVB_RELAY_EVENT_RELAY_CHANGED,
    EVB_RELAY_EVENT_BUTTON,
} evb_relay_event_id_t;

typedef enum {
    EVB_RELAY_RELAY_GROUP_ONBOARD = 0,
    EVB_RELAY_RELAY_GROUP_MODIO,
} evb_relay_relay_group_t;

typedef struct {
    uint8_t id;
    bool state;
    uint64_t ts_ms;
} evb_relay_digital_input_event_t;

typedef struct {
    uint8_t id;
    uint16_t value;
    uint64_t ts_ms;
} evb_relay_analog_input_event_t;

typedef struct {
    evb_relay_relay_group_t group;
    uint8_t id;
    bool state;
    uint64_t ts_ms;
} evb_relay_relay_changed_event_t;

typedef struct {
    bool pressed;
    uint64_t ts_ms;
} evb_relay_button_event_t;

#ifdef __cplusplus
}
#endif

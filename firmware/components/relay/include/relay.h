#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RELAY_COUNT 2U

esp_err_t relay_init(void);
esp_err_t relay_set(uint8_t relay_id, bool state);
esp_err_t relay_get(uint8_t relay_id, bool *out_state);
esp_err_t relay_toggle(uint8_t relay_id);

#ifdef UNIT_TEST
void relay_reset_for_testing(void);
#endif

#ifdef __cplusplus
}
#endif

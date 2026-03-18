#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "mod_io.h"

#ifdef __cplusplus
extern "C" {
#endif

#define INPUT_MONITOR_DEFAULT_ANALOG_CHANGE_THRESHOLD 8U
#define INPUT_MONITOR_DEFAULT_BUTTON_DEBOUNCE_MS 50U

typedef struct {
    bool modio_present;
    bool sample_valid;
    uint8_t digital_mask;
    uint16_t analog_values[MOD_IO_ANALOG_INPUT_COUNT];
    uint64_t sample_ts_ms;
} input_monitor_snapshot_t;

esp_err_t input_monitor_start(void);
esp_err_t input_monitor_get_snapshot(input_monitor_snapshot_t *out);

#if defined(UNIT_TEST) || defined(INPUT_MONITOR_ENABLE_TESTING_API)
esp_err_t input_monitor_poll_once_for_testing(void);
void input_monitor_reset_for_testing(void);
#endif

#ifdef __cplusplus
}
#endif

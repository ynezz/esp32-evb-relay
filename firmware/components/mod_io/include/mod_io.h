#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MOD_IO_I2C_ADDRESS 0x58U
#define MOD_IO_RELAY_COUNT 4U
#define MOD_IO_DIGITAL_INPUT_COUNT 4U
#define MOD_IO_ANALOG_INPUT_COUNT 4U
#define MOD_IO_RELAY_MASK_ALL 0x0FU
#define MOD_IO_DIGITAL_INPUT_MASK_ALL ((uint8_t)((1U << MOD_IO_DIGITAL_INPUT_COUNT) - 1U))

typedef enum {
    MOD_IO_RELAY_SYNC_ABSENT = 0,
    MOD_IO_RELAY_SYNC_SYNCHRONIZED,
} mod_io_relay_sync_t;

typedef struct {
    bool present;
    mod_io_relay_sync_t relay_sync;
    uint8_t relay_mask;
} mod_io_status_t;

esp_err_t mod_io_init(i2c_master_bus_handle_t bus_handle);
esp_err_t mod_io_probe(void);
bool mod_io_is_present(void);

esp_err_t mod_io_get_status(mod_io_status_t *out);
esp_err_t mod_io_get_relays(uint8_t *out_mask, mod_io_relay_sync_t *out_sync);

esp_err_t mod_io_set_relays(uint8_t relay_mask);
esp_err_t mod_io_set_relay(uint8_t relay_id, bool state);
esp_err_t mod_io_toggle_relay(uint8_t relay_id, bool *out_state);

esp_err_t mod_io_read_digital_inputs(uint8_t *out_mask);
esp_err_t mod_io_read_analog_input(uint8_t input_id, uint16_t *out_value);
esp_err_t mod_io_read_analog_inputs(uint16_t out_values[MOD_IO_ANALOG_INPUT_COUNT]);

const char *mod_io_relay_sync_to_string(mod_io_relay_sync_t relay_sync);

#ifdef UNIT_TEST
void mod_io_reset_for_testing(void);
#endif

#ifdef __cplusplus
}
#endif

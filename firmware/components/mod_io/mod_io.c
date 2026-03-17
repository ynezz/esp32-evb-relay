#include "mod_io.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define MOD_IO_I2C_TIMEOUT_MS 100
#define MOD_IO_RELAY_WRITE_COMMAND 0x10U
#define MOD_IO_DIGITAL_INPUT_READ_COMMAND 0x20U
#define MOD_IO_ANALOG_INPUT_BASE_COMMAND 0x30U

typedef struct {
    i2c_master_bus_handle_t bus_handle;
    i2c_master_dev_handle_t device_handle;
    bool initialized;
    bool present;
    mod_io_relay_sync_t relay_sync;
    uint8_t relay_mask;
} mod_io_state_t;

static const char *TAG = "mod_io";

static StaticSemaphore_t s_lock_buffer;
static SemaphoreHandle_t s_lock;
static mod_io_state_t s_state = {
    .bus_handle = NULL,
    .device_handle = NULL,
    .initialized = false,
    .present = false,
    .relay_sync = MOD_IO_RELAY_SYNC_ABSENT,
    .relay_mask = 0,
};

static esp_err_t mod_io_ensure_lock(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buffer);
    }

    return (s_lock != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t mod_io_lock(void)
{
    ESP_RETURN_ON_ERROR(mod_io_ensure_lock(), TAG, "Failed to create mutex");

    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

static void mod_io_unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

static esp_err_t mod_io_require_initialized_locked(void)
{
    return s_state.initialized ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t mod_io_validate_relay_mask(uint8_t relay_mask)
{
    return ((relay_mask & ~MOD_IO_RELAY_MASK_ALL) == 0U) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static esp_err_t mod_io_validate_relay_id(uint8_t relay_id)
{
    if ((relay_id == 0U) || (relay_id > MOD_IO_RELAY_COUNT)) {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static esp_err_t mod_io_validate_input_id(uint8_t input_id)
{
    if ((input_id == 0U) || (input_id > MOD_IO_ANALOG_INPUT_COUNT)) {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static uint64_t mod_io_timestamp_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000LL);
}

static void mod_io_mark_absent_locked(void)
{
    s_state.present = false;
    s_state.relay_sync = MOD_IO_RELAY_SYNC_ABSENT;
    s_state.relay_mask = 0;
}

static void mod_io_mark_present_locked(void)
{
    if (!s_state.present) {
        s_state.relay_mask = 0;
        s_state.relay_sync = MOD_IO_RELAY_SYNC_UNKNOWN;
    }

    s_state.present = true;
}

static esp_err_t mod_io_probe_locked(void)
{
    esp_err_t err;
    bool was_present;

    ESP_RETURN_ON_ERROR(mod_io_require_initialized_locked(), TAG, "MOD-IO is not initialized");

    was_present = s_state.present;
    err = i2c_master_probe(s_state.bus_handle, MOD_IO_I2C_ADDRESS, MOD_IO_I2C_TIMEOUT_MS);
    if (err == ESP_OK) {
        mod_io_mark_present_locked();
        if (!was_present) {
            ESP_LOGI(TAG, "Detected MOD-IO at 0x%02X; relay sync state is now unknown",
                     MOD_IO_I2C_ADDRESS);
        }
        return ESP_OK;
    }

    mod_io_mark_absent_locked();
    if ((err == ESP_ERR_NOT_FOUND) && was_present) {
        ESP_LOGW(TAG, "MOD-IO disappeared from the I2C bus; relay sync state reset to absent");
    }

    return err;
}

static esp_err_t mod_io_require_present_locked(void)
{
    ESP_RETURN_ON_ERROR(mod_io_require_initialized_locked(), TAG, "MOD-IO is not initialized");

    return s_state.present ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t mod_io_reconcile_after_transaction_failure_locked(esp_err_t transaction_err)
{
    esp_err_t probe_err;

    if (transaction_err == ESP_OK) {
        return ESP_OK;
    }

    probe_err = mod_io_probe_locked();
    if (probe_err == ESP_ERR_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (probe_err != ESP_OK) {
        return probe_err;
    }

    return transaction_err;
}

static uint16_t mod_io_decode_analog_sample(const uint8_t raw_bytes[2])
{
    uint8_t reversed_low_byte = raw_bytes[0];
    uint16_t value = 0;

    for (uint8_t bit_index = 0; bit_index < 8U; ++bit_index) {
        if ((reversed_low_byte & 0x80U) != 0U) {
            value |= (uint16_t)(1U << bit_index);
        }
        reversed_low_byte <<= 1U;
    }

    if ((raw_bytes[1] & 0x02U) != 0U) {
        value |= (1U << 8);
    }
    if ((raw_bytes[1] & 0x01U) != 0U) {
        value |= (1U << 9);
    }

    return value;
}

static esp_err_t mod_io_read_analog_input_locked(uint8_t input_id, uint16_t *out_value)
{
    uint8_t command;
    uint8_t raw_value[2];
    esp_err_t err;

    ESP_RETURN_ON_ERROR(mod_io_validate_input_id(input_id), TAG, "Invalid analog input id");
    ESP_RETURN_ON_FALSE(out_value != NULL, ESP_ERR_INVALID_ARG, TAG, "Output value is required");
    ESP_RETURN_ON_ERROR(mod_io_require_present_locked(), TAG, "MOD-IO is not present");

    command = (uint8_t)(MOD_IO_ANALOG_INPUT_BASE_COMMAND + (input_id - 1U));
    err = i2c_master_transmit_receive(s_state.device_handle, &command, sizeof(command), raw_value,
                                      sizeof(raw_value), MOD_IO_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return mod_io_reconcile_after_transaction_failure_locked(err);
    }

    *out_value = mod_io_decode_analog_sample(raw_value);
    return ESP_OK;
}

const char *mod_io_relay_sync_to_string(mod_io_relay_sync_t relay_sync)
{
    switch (relay_sync) {
    case MOD_IO_RELAY_SYNC_ABSENT:
        return "absent";
    case MOD_IO_RELAY_SYNC_UNKNOWN:
        return "unknown";
    case MOD_IO_RELAY_SYNC_SYNCHRONIZED:
        return "synchronized";
    default:
        return "unknown";
    }
}

esp_err_t mod_io_init(i2c_master_bus_handle_t bus_handle)
{
    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = MOD_IO_I2C_ADDRESS,
        .scl_speed_hz = 100000U,
        .scl_wait_us = 0,
        .flags.disable_ack_check = false,
    };
    esp_err_t err;
    esp_err_t probe_err;

    ESP_RETURN_ON_FALSE(bus_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "I2C bus handle is required");
    ESP_RETURN_ON_ERROR(mod_io_lock(), TAG, "Failed to lock MOD-IO state");

    if (s_state.initialized) {
        err = (s_state.bus_handle == bus_handle) ? ESP_OK : ESP_ERR_INVALID_STATE;
        mod_io_unlock();
        return err;
    }

    err = i2c_master_bus_add_device(bus_handle, &device_config, &s_state.device_handle);
    if (err != ESP_OK) {
        mod_io_unlock();
        return err;
    }

    s_state.bus_handle = bus_handle;
    s_state.initialized = true;
    mod_io_mark_absent_locked();

    probe_err = mod_io_probe_locked();
    if ((probe_err != ESP_OK) && (probe_err != ESP_ERR_NOT_FOUND)) {
        i2c_master_bus_rm_device(s_state.device_handle);
        memset(&s_state, 0, sizeof(s_state));
        s_state.relay_sync = MOD_IO_RELAY_SYNC_ABSENT;
        mod_io_unlock();
        return probe_err;
    }

    if (probe_err == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "MOD-IO not detected at 0x%02X; continuing without expansion board",
                 MOD_IO_I2C_ADDRESS);
    }

    mod_io_unlock();
    return ESP_OK;
}

esp_err_t mod_io_probe(void)
{
    esp_err_t err;

    ESP_RETURN_ON_ERROR(mod_io_lock(), TAG, "Failed to lock MOD-IO state");
    err = mod_io_probe_locked();
    mod_io_unlock();
    return err;
}

bool mod_io_is_present(void)
{
    bool present;

    if (mod_io_lock() != ESP_OK) {
        return false;
    }

    present = s_state.present;
    mod_io_unlock();
    return present;
}

esp_err_t mod_io_get_status(mod_io_status_t *out)
{
    esp_err_t err;

    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "Status output buffer is required");
    ESP_RETURN_ON_ERROR(mod_io_lock(), TAG, "Failed to lock MOD-IO state");

    err = mod_io_require_initialized_locked();
    if (err == ESP_OK) {
        out->present = s_state.present;
        out->relay_sync = s_state.relay_sync;
        out->relay_mask = s_state.relay_mask;
    }

    mod_io_unlock();
    return err;
}

esp_err_t mod_io_get_relays(uint8_t *out_mask, mod_io_relay_sync_t *out_sync)
{
    esp_err_t err;

    ESP_RETURN_ON_FALSE(out_mask != NULL, ESP_ERR_INVALID_ARG, TAG, "Relay mask output is required");
    ESP_RETURN_ON_ERROR(mod_io_lock(), TAG, "Failed to lock MOD-IO state");

    err = mod_io_require_initialized_locked();
    if (err == ESP_OK) {
        *out_mask = s_state.relay_mask;
        if (out_sync != NULL) {
            *out_sync = s_state.relay_sync;
        }
    }

    mod_io_unlock();
    return err;
}

esp_err_t mod_io_set_relays(uint8_t relay_mask)
{
    uint8_t command_buffer[2];
    esp_err_t err;

    ESP_RETURN_ON_ERROR(mod_io_validate_relay_mask(relay_mask), TAG, "Invalid relay mask");
    ESP_RETURN_ON_ERROR(mod_io_lock(), TAG, "Failed to lock MOD-IO state");
    err = mod_io_require_present_locked();
    if (err != ESP_OK) {
        mod_io_unlock();
        return err;
    }

    command_buffer[0] = MOD_IO_RELAY_WRITE_COMMAND;
    command_buffer[1] = relay_mask;

    err = i2c_master_transmit(s_state.device_handle, command_buffer, sizeof(command_buffer),
                              MOD_IO_I2C_TIMEOUT_MS);
    if (err == ESP_OK) {
        s_state.relay_mask = relay_mask;
        s_state.relay_sync = MOD_IO_RELAY_SYNC_SYNCHRONIZED;
        ESP_LOGD(TAG, "Set MOD-IO relays to mask=0x%02X at ts_ms=%llu", relay_mask,
                 (unsigned long long)mod_io_timestamp_ms());
    } else {
        err = mod_io_reconcile_after_transaction_failure_locked(err);
    }

    mod_io_unlock();
    return err;
}

esp_err_t mod_io_set_relay(uint8_t relay_id, bool state)
{
    uint8_t relay_mask;
    esp_err_t err;

    ESP_RETURN_ON_ERROR(mod_io_validate_relay_id(relay_id), TAG, "Invalid relay id");
    ESP_RETURN_ON_ERROR(mod_io_lock(), TAG, "Failed to lock MOD-IO state");
    err = mod_io_require_present_locked();
    if (err != ESP_OK) {
        mod_io_unlock();
        return err;
    }

    if (s_state.relay_sync != MOD_IO_RELAY_SYNC_SYNCHRONIZED) {
        mod_io_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    relay_mask = s_state.relay_mask;
    if (state) {
        relay_mask |= (uint8_t)(1U << (relay_id - 1U));
    } else {
        relay_mask &= (uint8_t)~(1U << (relay_id - 1U));
    }

    mod_io_unlock();
    return mod_io_set_relays(relay_mask);
}

esp_err_t mod_io_read_digital_inputs(uint8_t *out_mask)
{
    const uint8_t command = MOD_IO_DIGITAL_INPUT_READ_COMMAND;
    uint8_t read_value = 0;
    esp_err_t err;

    ESP_RETURN_ON_FALSE(out_mask != NULL, ESP_ERR_INVALID_ARG, TAG, "Input mask output is required");
    ESP_RETURN_ON_ERROR(mod_io_lock(), TAG, "Failed to lock MOD-IO state");
    err = mod_io_require_present_locked();
    if (err != ESP_OK) {
        mod_io_unlock();
        return err;
    }

    err = i2c_master_transmit_receive(s_state.device_handle, &command, sizeof(command), &read_value,
                                      sizeof(read_value), MOD_IO_I2C_TIMEOUT_MS);
    if (err == ESP_OK) {
        *out_mask = (uint8_t)(read_value & MOD_IO_RELAY_MASK_ALL);
    } else {
        err = mod_io_reconcile_after_transaction_failure_locked(err);
    }

    mod_io_unlock();
    return err;
}

esp_err_t mod_io_read_analog_input(uint8_t input_id, uint16_t *out_value)
{
    esp_err_t err;

    ESP_RETURN_ON_ERROR(mod_io_lock(), TAG, "Failed to lock MOD-IO state");
    err = mod_io_read_analog_input_locked(input_id, out_value);
    mod_io_unlock();
    return err;
}

esp_err_t mod_io_read_analog_inputs(uint16_t out_values[MOD_IO_ANALOG_INPUT_COUNT])
{
    esp_err_t err;

    ESP_RETURN_ON_FALSE(out_values != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Analog input output buffer is required");
    ESP_RETURN_ON_ERROR(mod_io_lock(), TAG, "Failed to lock MOD-IO state");
    err = mod_io_require_present_locked();
    if (err != ESP_OK) {
        mod_io_unlock();
        return err;
    }

    for (uint8_t input_id = 1U; input_id <= MOD_IO_ANALOG_INPUT_COUNT; ++input_id) {
        err = mod_io_read_analog_input_locked(input_id, &out_values[input_id - 1U]);
        if (err != ESP_OK) {
            mod_io_unlock();
            return err;
        }
    }

    mod_io_unlock();
    return ESP_OK;
}

#ifdef UNIT_TEST
void mod_io_reset_for_testing(void)
{
    memset(&s_state, 0, sizeof(s_state));
    s_lock = NULL;
    memset(&s_lock_buffer, 0, sizeof(s_lock_buffer));
}
#endif

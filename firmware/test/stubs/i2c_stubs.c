#include "i2c_stubs.h"

#include <string.h>

#define I2C_STUB_MAX_TRANSACTION_SIZE 64
#define I2C_STUB_MAX_QUEUED_READS 32

typedef struct {
    uint8_t data[I2C_STUB_MAX_TRANSACTION_SIZE];
    size_t data_len;
} i2c_stub_read_entry_t;

static struct {
    int reserved;
} s_bus_instance;

static struct {
    int reserved;
} s_device_instance;

static i2c_master_bus_config_t s_last_bus_config;
static i2c_device_config_t s_last_device_config;
static esp_err_t s_probe_result = ESP_ERR_NOT_FOUND;
static esp_err_t s_transmit_result = ESP_OK;
static esp_err_t s_transmit_receive_result = ESP_OK;
static uint8_t s_last_transaction[I2C_STUB_MAX_TRANSACTION_SIZE];
static size_t s_last_transaction_size;
static uint8_t s_read_data[I2C_STUB_MAX_TRANSACTION_SIZE];
static size_t s_read_data_size;
static i2c_stub_read_entry_t s_read_queue[I2C_STUB_MAX_QUEUED_READS];
static size_t s_read_queue_len;
static size_t s_read_queue_index;

static esp_err_t i2c_stub_record_transaction(const uint8_t *data, size_t data_len)
{
    if (data_len > sizeof(s_last_transaction)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if ((data_len > 0U) && (data == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (data_len > 0U) {
        memcpy(s_last_transaction, data, data_len);
    }
    s_last_transaction_size = data_len;
    return ESP_OK;
}

void i2c_stub_reset(void)
{
    memset(&s_last_bus_config, 0, sizeof(s_last_bus_config));
    memset(&s_last_device_config, 0, sizeof(s_last_device_config));
    memset(s_last_transaction, 0, sizeof(s_last_transaction));
    memset(s_read_data, 0, sizeof(s_read_data));
    memset(s_read_queue, 0, sizeof(s_read_queue));
    s_last_transaction_size = 0U;
    s_read_data_size = 0U;
    s_read_queue_len = 0U;
    s_read_queue_index = 0U;
    s_probe_result = ESP_ERR_NOT_FOUND;
    s_transmit_result = ESP_OK;
    s_transmit_receive_result = ESP_OK;
}

void i2c_stub_set_probe_result(esp_err_t result)
{
    s_probe_result = result;
}

void i2c_stub_set_transmit_result(esp_err_t result)
{
    s_transmit_result = result;
}

void i2c_stub_set_transmit_receive_result(esp_err_t result)
{
    s_transmit_receive_result = result;
}

esp_err_t i2c_stub_set_read_data(const uint8_t *data, size_t data_len)
{
    if (data_len > sizeof(s_read_data)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if ((data_len > 0U) && (data == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (data_len > 0U) {
        memcpy(s_read_data, data, data_len);
    }
    s_read_data_size = data_len;
    return ESP_OK;
}

esp_err_t i2c_stub_queue_read_data(const uint8_t *data, size_t data_len)
{
    if (s_read_queue_len >= I2C_STUB_MAX_QUEUED_READS) {
        return ESP_ERR_NO_MEM;
    }
    if (data_len > I2C_STUB_MAX_TRANSACTION_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    if ((data_len > 0U) && (data == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (data_len > 0U) {
        memcpy(s_read_queue[s_read_queue_len].data, data, data_len);
    }
    s_read_queue[s_read_queue_len].data_len = data_len;
    ++s_read_queue_len;
    return ESP_OK;
}

const uint8_t *i2c_stub_get_last_transaction(size_t *out_size)
{
    if (out_size != NULL) {
        *out_size = s_last_transaction_size;
    }

    return s_last_transaction;
}

const i2c_master_bus_config_t *i2c_stub_get_last_bus_config(void)
{
    return &s_last_bus_config;
}

const i2c_device_config_t *i2c_stub_get_last_device_config(void)
{
    return &s_last_device_config;
}

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *bus_config,
                             i2c_master_bus_handle_t *ret_bus_handle)
{
    if ((bus_config == NULL) || (ret_bus_handle == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    s_last_bus_config = *bus_config;
    *ret_bus_handle = &s_bus_instance;
    return ESP_OK;
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus_handle,
                                    const i2c_device_config_t *dev_config,
                                    i2c_master_dev_handle_t *ret_handle)
{
    if ((bus_handle == NULL) || (dev_config == NULL) || (ret_handle == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    s_last_device_config = *dev_config;
    *ret_handle = &s_device_instance;
    return ESP_OK;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t dev_handle)
{
    return (dev_handle != NULL) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus_handle, uint16_t address, int timeout_ms)
{
    (void)address;
    (void)timeout_ms;
    return (bus_handle != NULL) ? s_probe_result : ESP_ERR_INVALID_ARG;
}

esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev_handle,
                              const uint8_t *write_buffer,
                              size_t write_size,
                              int timeout_ms)
{
    esp_err_t err;

    (void)timeout_ms;

    if (dev_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = i2c_stub_record_transaction(write_buffer, write_size);
    if (err != ESP_OK) {
        return err;
    }

    return s_transmit_result;
}

esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev_handle,
                                      const uint8_t *write_buffer,
                                      size_t write_size,
                                      uint8_t *read_buffer,
                                      size_t read_size,
                                      int timeout_ms)
{
    esp_err_t err;
    size_t bytes_to_copy;

    (void)timeout_ms;

    if ((dev_handle == NULL) || ((read_size > 0U) && (read_buffer == NULL))) {
        return ESP_ERR_INVALID_ARG;
    }

    err = i2c_stub_record_transaction(write_buffer, write_size);
    if (err != ESP_OK) {
        return err;
    }

    if (s_transmit_receive_result != ESP_OK) {
        return s_transmit_receive_result;
    }

    if (s_read_queue_index < s_read_queue_len) {
        size_t queued_size = s_read_queue[s_read_queue_index].data_len;

        bytes_to_copy = (read_size < queued_size) ? read_size : queued_size;
        if (bytes_to_copy > 0U) {
            memcpy(read_buffer, s_read_queue[s_read_queue_index].data, bytes_to_copy);
        }
        if (read_size > bytes_to_copy) {
            memset(read_buffer + bytes_to_copy, 0, read_size - bytes_to_copy);
        }
        ++s_read_queue_index;
        return ESP_OK;
    }

    bytes_to_copy = (read_size < s_read_data_size) ? read_size : s_read_data_size;
    if (bytes_to_copy > 0U) {
        memcpy(read_buffer, s_read_data, bytes_to_copy);
    }
    if (read_size > bytes_to_copy) {
        memset(read_buffer + bytes_to_copy, 0, read_size - bytes_to_copy);
    }

    return ESP_OK;
}

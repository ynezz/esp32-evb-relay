#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef int32_t i2c_port_num_t;
typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;
typedef int gpio_num_t;

typedef enum {
    I2C_CLK_SRC_DEFAULT = 0,
} i2c_clock_source_t;

typedef enum {
    I2C_ADDR_BIT_LEN_7 = 7,
} i2c_addr_bit_len_t;

typedef struct {
    i2c_port_num_t i2c_port;
    int sda_io_num;
    int scl_io_num;
    i2c_clock_source_t clk_source;
    uint8_t glitch_ignore_cnt;
    struct {
        bool enable_internal_pullup;
    } flags;
} i2c_master_bus_config_t;

typedef struct {
    i2c_addr_bit_len_t dev_addr_length;
    uint16_t device_address;
    uint32_t scl_speed_hz;
    uint32_t scl_wait_us;
    struct {
        bool disable_ack_check;
    } flags;
} i2c_device_config_t;

#define I2C_NUM_0 0
#define GPIO_NUM_13 13
#define GPIO_NUM_16 16
#define GPIO_NUM_18 18
#define GPIO_NUM_23 23
#define GPIO_NUM_32 32
#define GPIO_NUM_33 33
#define GPIO_NUM_34 34

void i2c_stub_reset(void);
void i2c_stub_set_probe_result(esp_err_t result);
void i2c_stub_set_transmit_result(esp_err_t result);
void i2c_stub_set_transmit_result_persistent(esp_err_t result);
void i2c_stub_set_transmit_receive_result(esp_err_t result);
esp_err_t i2c_stub_set_read_data(const uint8_t *data, size_t data_len);
esp_err_t i2c_stub_queue_read_data(const uint8_t *data, size_t data_len);
const uint8_t *i2c_stub_get_last_transaction(size_t *out_size);
const i2c_master_bus_config_t *i2c_stub_get_last_bus_config(void);
const i2c_device_config_t *i2c_stub_get_last_device_config(void);

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *bus_config,
                             i2c_master_bus_handle_t *ret_bus_handle);
esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t bus_handle);
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus_handle,
                                    const i2c_device_config_t *dev_config,
                                    i2c_master_dev_handle_t *ret_handle);
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t dev_handle);
esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus_handle, uint16_t address, int timeout_ms);
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev_handle,
                              const uint8_t *write_buffer,
                              size_t write_size,
                              int timeout_ms);
esp_err_t i2c_master_receive(i2c_master_dev_handle_t dev_handle,
                             uint8_t *read_buffer,
                             size_t read_size,
                             int timeout_ms);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev_handle,
                                      const uint8_t *write_buffer,
                                      size_t write_size,
                                      uint8_t *read_buffer,
                                      size_t read_size,
                                      int timeout_ms);

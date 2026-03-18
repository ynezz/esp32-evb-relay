#include "board.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "board";

static i2c_master_bus_handle_t s_i2c_bus_handle;
static bool s_initialized;

esp_err_t board_init(void)
{
    const gpio_config_t button_config = {
        .pin_bit_mask = (1ULL << BOARD_BUTTON),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    const i2c_master_bus_config_t i2c_bus_config = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    if (s_initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(gpio_config(&button_config), TAG, "Failed to configure button GPIO");
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_bus_config, &s_i2c_bus_handle), TAG,
                        "Failed to initialize I2C master bus");

    s_initialized = true;
    ESP_LOGI(TAG,
             "Board initialized: relays=%d,%d i2c_port=%d sda=%d scl=%d button=%d i2c_speed_hz=%u",
             BOARD_RELAY1_GPIO, BOARD_RELAY2_GPIO, BOARD_I2C_PORT, BOARD_I2C_SDA, BOARD_I2C_SCL,
             BOARD_BUTTON, BOARD_I2C_SCL_SPEED_HZ);

    return ESP_OK;
}

i2c_master_bus_handle_t board_i2c_bus_handle(void)
{
    return s_i2c_bus_handle;
}

i2c_device_config_t board_i2c_device_config(uint16_t device_address)
{
    return (i2c_device_config_t) {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = device_address,
        .scl_speed_hz = BOARD_I2C_SCL_SPEED_HZ,
        .scl_wait_us = 0,
        .flags.disable_ack_check = false,
    };
}

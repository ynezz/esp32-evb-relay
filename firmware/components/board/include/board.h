#pragma once

#include "driver/i2c_master.h"
#include "driver/i2c_types.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOARD_RELAY1_GPIO GPIO_NUM_32
#define BOARD_RELAY2_GPIO GPIO_NUM_33
#define BOARD_I2C_SDA GPIO_NUM_13
#define BOARD_I2C_SCL GPIO_NUM_16
#define BOARD_ETH_MDC GPIO_NUM_23
#define BOARD_ETH_MDIO GPIO_NUM_18
#define BOARD_BUTTON GPIO_NUM_34

#define BOARD_I2C_PORT I2C_NUM_0
#define BOARD_I2C_SCL_SPEED_HZ 100000U

esp_err_t board_init(void);
i2c_master_bus_handle_t board_i2c_bus_handle(void);
i2c_device_config_t board_i2c_device_config(uint16_t device_address);

#if defined(UNIT_TEST) || defined(BOARD_ENABLE_TESTING_API)
void board_reset_for_testing(void);
#endif

#ifdef __cplusplus
}
#endif

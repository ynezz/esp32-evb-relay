#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef int32_t gpio_num_t;

typedef enum {
    GPIO_MODE_DISABLE = 0,
    GPIO_MODE_INPUT = 1,
    GPIO_MODE_OUTPUT = 2,
} gpio_mode_t;

typedef enum {
    GPIO_PULLUP_DISABLE = 0,
    GPIO_PULLUP_ENABLE = 1,
} gpio_pullup_t;

typedef enum {
    GPIO_PULLDOWN_DISABLE = 0,
    GPIO_PULLDOWN_ENABLE = 1,
} gpio_pulldown_t;

typedef enum {
    GPIO_INTR_DISABLE = 0,
} gpio_int_type_t;

typedef struct {
    uint64_t pin_bit_mask;
    gpio_mode_t mode;
    gpio_pullup_t pull_up_en;
    gpio_pulldown_t pull_down_en;
    gpio_int_type_t intr_type;
} gpio_config_t;

#define GPIO_NUM_13 13
#define GPIO_NUM_16 16
#define GPIO_NUM_18 18
#define GPIO_NUM_23 23
#define GPIO_NUM_32 32
#define GPIO_NUM_33 33
#define GPIO_NUM_34 34

void gpio_stub_reset(void);
bool gpio_stub_is_configured(gpio_num_t gpio_num);
gpio_mode_t gpio_stub_get_mode(gpio_num_t gpio_num);
uint32_t gpio_stub_get_level(gpio_num_t gpio_num);

esp_err_t gpio_config(const gpio_config_t *config);
esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level);

#include "gpio_stubs.h"

#include <string.h>

#define GPIO_STUB_MAX_PINS 64

typedef struct {
    bool configured;
    gpio_mode_t mode;
    uint32_t level;
} gpio_stub_pin_state_t;

static gpio_stub_pin_state_t s_gpio_pins[GPIO_STUB_MAX_PINS];

static bool gpio_stub_is_valid(gpio_num_t gpio_num)
{
    return (gpio_num >= 0) && (gpio_num < GPIO_STUB_MAX_PINS);
}

void gpio_stub_reset(void)
{
    memset(s_gpio_pins, 0, sizeof(s_gpio_pins));
}

bool gpio_stub_is_configured(gpio_num_t gpio_num)
{
    return gpio_stub_is_valid(gpio_num) ? s_gpio_pins[gpio_num].configured : false;
}

gpio_mode_t gpio_stub_get_mode(gpio_num_t gpio_num)
{
    return gpio_stub_is_valid(gpio_num) ? s_gpio_pins[gpio_num].mode : GPIO_MODE_DISABLE;
}

uint32_t gpio_stub_get_level(gpio_num_t gpio_num)
{
    return gpio_stub_is_valid(gpio_num) ? s_gpio_pins[gpio_num].level : 0U;
}

esp_err_t gpio_config(const gpio_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t pin = 0; pin < GPIO_STUB_MAX_PINS; ++pin) {
        if ((config->pin_bit_mask & (1ULL << pin)) == 0U) {
            continue;
        }

        s_gpio_pins[pin].configured = true;
        s_gpio_pins[pin].mode = config->mode;
    }

    return ESP_OK;
}

esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level)
{
    if (!gpio_stub_is_valid(gpio_num)) {
        return ESP_ERR_INVALID_ARG;
    }

    s_gpio_pins[gpio_num].level = level;
    return ESP_OK;
}

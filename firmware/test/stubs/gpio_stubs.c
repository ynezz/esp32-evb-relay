#include "gpio_stubs.h"

#include <string.h>

#define GPIO_STUB_MAX_PINS 64

typedef struct {
    bool configured;
    gpio_mode_t mode;
    gpio_int_type_t intr_type;
    uint32_t level;
    gpio_isr_t isr_handler;
    void *isr_args;
} gpio_stub_pin_state_t;

static gpio_stub_pin_state_t s_gpio_pins[GPIO_STUB_MAX_PINS];
static bool s_isr_service_installed;

static bool gpio_stub_is_valid(gpio_num_t gpio_num)
{
    return (gpio_num >= 0) && (gpio_num < GPIO_STUB_MAX_PINS);
}

void gpio_stub_reset(void)
{
    memset(s_gpio_pins, 0, sizeof(s_gpio_pins));
    s_isr_service_installed = false;
}

bool gpio_stub_is_configured(gpio_num_t gpio_num)
{
    return gpio_stub_is_valid(gpio_num) ? s_gpio_pins[gpio_num].configured : false;
}

gpio_mode_t gpio_stub_get_mode(gpio_num_t gpio_num)
{
    return gpio_stub_is_valid(gpio_num) ? s_gpio_pins[gpio_num].mode : GPIO_MODE_DISABLE;
}

gpio_int_type_t gpio_stub_get_intr_type(gpio_num_t gpio_num)
{
    return gpio_stub_is_valid(gpio_num) ? s_gpio_pins[gpio_num].intr_type : GPIO_INTR_DISABLE;
}

bool gpio_stub_has_isr_handler(gpio_num_t gpio_num)
{
    return gpio_stub_is_valid(gpio_num) ? (s_gpio_pins[gpio_num].isr_handler != NULL) : false;
}

uint32_t gpio_stub_get_level(gpio_num_t gpio_num)
{
    return gpio_stub_is_valid(gpio_num) ? s_gpio_pins[gpio_num].level : 0U;
}

void gpio_stub_set_input_level(gpio_num_t gpio_num, uint32_t level)
{
    if (gpio_stub_is_valid(gpio_num)) {
        s_gpio_pins[gpio_num].level = level;
    }
}

void gpio_stub_trigger_isr(gpio_num_t gpio_num)
{
    if (!gpio_stub_is_valid(gpio_num) || (s_gpio_pins[gpio_num].isr_handler == NULL)) {
        return;
    }

    s_gpio_pins[gpio_num].isr_handler(s_gpio_pins[gpio_num].isr_args);
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
        s_gpio_pins[pin].intr_type = config->intr_type;
    }

    return ESP_OK;
}

esp_err_t gpio_install_isr_service(int intr_alloc_flags)
{
    (void)intr_alloc_flags;

    if (s_isr_service_installed) {
        return ESP_ERR_INVALID_STATE;
    }

    s_isr_service_installed = true;
    return ESP_OK;
}

esp_err_t gpio_set_intr_type(gpio_num_t gpio_num, gpio_int_type_t intr_type)
{
    if (!gpio_stub_is_valid(gpio_num)) {
        return ESP_ERR_INVALID_ARG;
    }

    s_gpio_pins[gpio_num].intr_type = intr_type;
    return ESP_OK;
}

esp_err_t gpio_isr_handler_add(gpio_num_t gpio_num, gpio_isr_t isr_handler, void *args)
{
    if (!s_isr_service_installed) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!gpio_stub_is_valid(gpio_num) || (isr_handler == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    s_gpio_pins[gpio_num].isr_handler = isr_handler;
    s_gpio_pins[gpio_num].isr_args = args;
    return ESP_OK;
}

esp_err_t gpio_isr_handler_remove(gpio_num_t gpio_num)
{
    if (!gpio_stub_is_valid(gpio_num)) {
        return ESP_ERR_INVALID_ARG;
    }

    s_gpio_pins[gpio_num].isr_handler = NULL;
    s_gpio_pins[gpio_num].isr_args = NULL;
    return ESP_OK;
}

int gpio_get_level(gpio_num_t gpio_num)
{
    return (int)gpio_stub_get_level(gpio_num);
}

esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level)
{
    if (!gpio_stub_is_valid(gpio_num)) {
        return ESP_ERR_INVALID_ARG;
    }

    s_gpio_pins[gpio_num].level = level;
    return ESP_OK;
}

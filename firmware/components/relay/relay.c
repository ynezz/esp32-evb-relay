#include "relay.h"

#include <string.h>

#include "board.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "rest_api_events.h"

#define RELAY_GPIO_LEVEL_OFF 0
#define RELAY_GPIO_LEVEL_ON 1

typedef struct {
    bool initialized;
    bool states[RELAY_COUNT];
} relay_state_t;

static const char *TAG = "relay";

static const gpio_num_t s_relay_gpios[RELAY_COUNT] = {
    BOARD_RELAY1_GPIO,
    BOARD_RELAY2_GPIO,
};

static StaticSemaphore_t s_lock_buffer;
static SemaphoreHandle_t s_lock;
static relay_state_t s_state;

static esp_err_t relay_ensure_lock(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buffer);
    }

    return (s_lock != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t relay_lock(void)
{
    ESP_RETURN_ON_ERROR(relay_ensure_lock(), TAG, "Failed to create relay mutex");

    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

static void relay_unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

static esp_err_t relay_require_initialized_locked(void)
{
    return s_state.initialized ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t relay_validate_id(uint8_t relay_id)
{
    if ((relay_id == 0U) || (relay_id > RELAY_COUNT)) {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static uint64_t relay_timestamp_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000LL);
}

static esp_err_t relay_apply_state_locked(uint8_t relay_id, bool state)
{
    return gpio_set_level(s_relay_gpios[relay_id - 1U],
                          state ? RELAY_GPIO_LEVEL_ON : RELAY_GPIO_LEVEL_OFF);
}

static esp_err_t relay_update_state_locked(uint8_t relay_id, bool state, bool *out_changed)
{
    ESP_RETURN_ON_FALSE(out_changed != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "State change output is required");

    if (s_state.states[relay_id - 1U] == state) {
        *out_changed = false;
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(relay_apply_state_locked(relay_id, state), TAG,
                        "Failed to drive relay GPIO");

    s_state.states[relay_id - 1U] = state;
    *out_changed = true;
    return ESP_OK;
}

static void relay_publish_change_event(uint8_t relay_id, bool state)
{
    evb_relay_relay_changed_event_t event = {
        .group = EVB_RELAY_RELAY_GROUP_ONBOARD,
        .id = relay_id,
        .state = state,
        .ts_ms = relay_timestamp_ms(),
    };
    esp_err_t err = esp_event_post(EVB_RELAY_EVENT, EVB_RELAY_EVENT_RELAY_CHANGED, &event,
                                   sizeof(event), 0);

    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGD(TAG,
                 "Skipping relay event for relay %u because the default event loop is not ready",
                 (unsigned)relay_id);
        return;
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to publish relay change event for relay %u: %s",
                 (unsigned)relay_id, esp_err_to_name(err));
    }
}

esp_err_t relay_init(void)
{
    const gpio_config_t relay_config = {
        .pin_bit_mask = (1ULL << BOARD_RELAY1_GPIO) | (1ULL << BOARD_RELAY2_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err;

    ESP_RETURN_ON_ERROR(relay_lock(), TAG, "Failed to lock relay state");

    if (s_state.initialized) {
        relay_unlock();
        return ESP_OK;
    }

    err = gpio_config(&relay_config);
    if (err != ESP_OK) {
        relay_unlock();
        return err;
    }

    err = gpio_set_level(BOARD_RELAY1_GPIO, RELAY_GPIO_LEVEL_OFF);
    if (err != ESP_OK) {
        relay_unlock();
        return err;
    }

    err = gpio_set_level(BOARD_RELAY2_GPIO, RELAY_GPIO_LEVEL_OFF);
    if (err != ESP_OK) {
        relay_unlock();
        return err;
    }

    memset(&s_state, 0, sizeof(s_state));
    s_state.initialized = true;
    relay_unlock();

    ESP_LOGI(TAG, "Initialized onboard relays on GPIO%d/GPIO%d with boot default OFF",
             BOARD_RELAY1_GPIO, BOARD_RELAY2_GPIO);
    return ESP_OK;
}

esp_err_t relay_set(uint8_t relay_id, bool state)
{
    bool changed = false;
    esp_err_t err;

    ESP_RETURN_ON_ERROR(relay_validate_id(relay_id), TAG, "Invalid relay id");
    ESP_RETURN_ON_ERROR(relay_lock(), TAG, "Failed to lock relay state");

    err = relay_require_initialized_locked();
    if (err != ESP_OK) {
        relay_unlock();
        return err;
    }

    err = relay_update_state_locked(relay_id, state, &changed);
    if (err != ESP_OK) {
        relay_unlock();
        return err;
    }

    relay_unlock();

    if (changed) {
        relay_publish_change_event(relay_id, state);
    }

    return ESP_OK;
}

esp_err_t relay_get(uint8_t relay_id, bool *out_state)
{
    esp_err_t err;

    ESP_RETURN_ON_ERROR(relay_validate_id(relay_id), TAG, "Invalid relay id");
    ESP_RETURN_ON_FALSE(out_state != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "Relay state output is required");
    ESP_RETURN_ON_ERROR(relay_lock(), TAG, "Failed to lock relay state");

    err = relay_require_initialized_locked();
    if (err == ESP_OK) {
        *out_state = s_state.states[relay_id - 1U];
    }

    relay_unlock();
    return err;
}

esp_err_t relay_toggle(uint8_t relay_id)
{
    bool changed = false;
    bool next_state;
    esp_err_t err;

    ESP_RETURN_ON_ERROR(relay_validate_id(relay_id), TAG, "Invalid relay id");
    ESP_RETURN_ON_ERROR(relay_lock(), TAG, "Failed to lock relay state");

    err = relay_require_initialized_locked();
    if (err != ESP_OK) {
        relay_unlock();
        return err;
    }

    next_state = !s_state.states[relay_id - 1U];
    err = relay_update_state_locked(relay_id, next_state, &changed);
    relay_unlock();
    if (err != ESP_OK) {
        return err;
    }

    if (changed) {
        relay_publish_change_event(relay_id, next_state);
    }

    return ESP_OK;
}

#ifdef UNIT_TEST
void relay_reset_for_testing(void)
{
    memset(&s_state, 0, sizeof(s_state));
    s_lock = NULL;
    memset(&s_lock_buffer, 0, sizeof(s_lock_buffer));
}
#endif

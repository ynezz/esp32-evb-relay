#include "auth.h"
#include "board.h"
#include "device_config.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "mod_io.h"
#include "relay.h"
#include "rest_api.h"

static const char *TAG = "main";

static rest_api_modio_sync_t app_modio_sync_to_rest_api(mod_io_relay_sync_t relay_sync)
{
    switch (relay_sync) {
    case MOD_IO_RELAY_SYNC_ABSENT:
        return REST_API_MODIO_SYNC_ABSENT;
    case MOD_IO_RELAY_SYNC_SYNCHRONIZED:
        return REST_API_MODIO_SYNC_SYNCHRONIZED;
    default:
        return REST_API_MODIO_SYNC_ABSENT;
    }
}

static esp_err_t app_status_provider(rest_api_status_view_t *status, void *ctx)
{
    mod_io_status_t mod_io_status;
    esp_err_t err;

    (void)ctx;
    err = mod_io_probe();
    if ((err != ESP_OK) && (err != ESP_ERR_NOT_FOUND)) {
        ESP_LOGW(TAG, "Failed to refresh MOD-IO state from readback: %s", esp_err_to_name(err));
    }

    err = mod_io_get_status(&mod_io_status);
    if (err != ESP_OK) {
        return err;
    }

    status->modio_present = mod_io_status.present;
    status->modio_sync = app_modio_sync_to_rest_api(mod_io_status.relay_sync);
    return ESP_OK;
}

void app_main(void)
{
    rest_api_config_t api_config = {
        .port = 80,
        .auth_handler = auth_check,
        .status_provider = app_status_provider,
    };
    esp_err_t err;

    err = esp_event_loop_create_default();
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        ESP_LOGE(TAG, "Failed to create default event loop: %s", esp_err_to_name(err));
        return;
    }

    err = device_config_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize device config: %s", esp_err_to_name(err));
        return;
    }

    err = board_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize board: %s", esp_err_to_name(err));
        return;
    }

    err = auth_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize auth: %s", esp_err_to_name(err));
        return;
    }

    err = relay_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize onboard relays: %s", esp_err_to_name(err));
        return;
    }

    err = mod_io_init(board_i2c_bus_handle());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize MOD-IO component: %s", esp_err_to_name(err));
        return;
    }

    err = rest_api_start(&api_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start REST API: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "REST API listening on /api/v1");
}

#include <string.h>

#include "auth.h"
#include "board.h"
#include "device_config.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input_monitor.h"
#include "mod_io.h"
#include "network.h"
#include "ota.h"
#include "relay.h"
#include "rest_api.h"

static const char *TAG = "main";
static const uint32_t APP_NETWORK_STATUS_POLL_INTERVAL_MS = 1000U;

static rest_api_network_transport_t app_status_transport_from_network(network_transport_t transport)
{
    switch (transport) {
    case NETWORK_TRANSPORT_ETHERNET:
        return REST_API_NETWORK_TRANSPORT_ETHERNET;
    case NETWORK_TRANSPORT_WIFI:
        return REST_API_NETWORK_TRANSPORT_WIFI;
    case NETWORK_TRANSPORT_NONE:
    default:
        return REST_API_NETWORK_TRANSPORT_NONE;
    }
}

static esp_err_t app_status_provider(rest_api_status_view_t *status, void *ctx)
{
    mod_io_status_t mod_io_status;
    network_status_t network_status;
    esp_err_t err;

    (void)ctx;
    err = network_get_status(&network_status);
    if (err != ESP_OK) {
        return err;
    }

    err = mod_io_probe();
    if ((err != ESP_OK) && (err != ESP_ERR_NOT_FOUND)) {
        ESP_LOGW(TAG, "Failed to refresh MOD-IO presence: %s", esp_err_to_name(err));
    }

    err = mod_io_get_status(&mod_io_status);
    if (err != ESP_OK) {
        return err;
    }

    status->network.transport = app_status_transport_from_network(network_status.transport);
    status->network.connected = network_status.connected;
    memcpy(status->network.hostname, network_status.hostname, sizeof(status->network.hostname));
    memcpy(status->network.ip, network_status.ip, sizeof(status->network.ip));
    memcpy(status->network.netmask, network_status.netmask, sizeof(status->network.netmask));
    memcpy(status->network.gateway, network_status.gateway, sizeof(status->network.gateway));
    status->modio_present = mod_io_status.present;
    status->modio_sync = rest_api_modio_sync_from_driver(mod_io_status.relay_sync);
    return ESP_OK;
}

static esp_err_t app_wait_for_network_connectivity(void)
{
    network_status_t network_status;
    esp_err_t err;

    err = network_wait_for_ip(NETWORK_DEFAULT_WAIT_FOR_IP_TIMEOUT_MS);
    if (err == ESP_OK) {
        return ESP_OK;
    }

    if (err != ESP_ERR_TIMEOUT) {
        return err;
    }

    ESP_LOGW(TAG, "Initial network wait timed out; continuing to wait for connectivity");

    for (;;) {
        err = network_get_status(&network_status);
        if (err != ESP_OK) {
            return err;
        }

        if (network_status.connected) {
            return ESP_OK;
        }

        // The initial wait may already have triggered transport fallback, so
        // keep polling shared status instead of re-entering wait side effects.
        vTaskDelay(pdMS_TO_TICKS(APP_NETWORK_STATUS_POLL_INTERVAL_MS));
    }
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

    err = input_monitor_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start input monitor: %s", esp_err_to_name(err));
        return;
    }

    err = auth_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize auth: %s", esp_err_to_name(err));
        return;
    }

    err = network_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize Ethernet networking: %s", esp_err_to_name(err));
        return;
    }

    err = ota_confirm_running_image_if_pending();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to confirm the running OTA image: %s", esp_err_to_name(err));
        return;
    }

    err = app_wait_for_network_connectivity();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed while waiting for network connectivity: %s", esp_err_to_name(err));
        return;
    }

    err = network_register_mdns_service(api_config.port);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register mDNS service: %s", esp_err_to_name(err));
        return;
    }

    err = rest_api_start(&api_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start REST API: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "REST API listening on /api/v1");
}

#include "main_startup_stubs.h"

#include <stdio.h>
#include <string.h>

#define MAIN_STARTUP_MAX_CALLS 32U

static const uintptr_t BOARD_BUS_HANDLE_VALUE = 0x1234U;

typedef struct {
    main_startup_call_t calls[MAIN_STARTUP_MAX_CALLS];
    size_t call_count;
    esp_err_t event_loop_result;
    esp_err_t device_config_init_result;
    esp_err_t board_init_result;
    esp_err_t auth_init_result;
    esp_err_t relay_init_result;
    esp_err_t mod_io_init_result;
    esp_err_t input_monitor_start_result;
    esp_err_t mod_io_probe_result;
    esp_err_t mod_io_get_status_result;
    esp_err_t network_init_result;
    esp_err_t ota_confirm_result;
    esp_err_t network_wait_result;
    esp_err_t network_register_mdns_result;
    esp_err_t network_get_status_result;
    esp_err_t rest_api_start_result;
    uint32_t last_wait_timeout_ms;
    uint16_t last_mdns_port;
    network_status_t network_status;
    mod_io_status_t mod_io_status;
    rest_api_config_t last_rest_api_config;
    bool rest_api_config_captured;
} main_startup_stub_state_t;

static main_startup_stub_state_t s_state;

static void main_startup_stub_record_call(main_startup_call_t call)
{
    if (s_state.call_count < MAIN_STARTUP_MAX_CALLS) {
        s_state.calls[s_state.call_count++] = call;
    }
}

void main_startup_stub_reset(void)
{
    memset(&s_state, 0, sizeof(s_state));
    s_state.event_loop_result = ESP_OK;
    s_state.device_config_init_result = ESP_OK;
    s_state.board_init_result = ESP_OK;
    s_state.auth_init_result = ESP_OK;
    s_state.relay_init_result = ESP_OK;
    s_state.mod_io_init_result = ESP_OK;
    s_state.input_monitor_start_result = ESP_OK;
    s_state.mod_io_probe_result = ESP_OK;
    s_state.mod_io_get_status_result = ESP_OK;
    s_state.network_init_result = ESP_OK;
    s_state.ota_confirm_result = ESP_OK;
    s_state.network_wait_result = ESP_OK;
    s_state.network_register_mdns_result = ESP_OK;
    s_state.network_get_status_result = ESP_OK;
    s_state.rest_api_start_result = ESP_OK;

    s_state.network_status.connected = true;
    (void)snprintf(s_state.network_status.hostname,
                   sizeof(s_state.network_status.hostname),
                   "esp32-evb-relay");
    (void)snprintf(s_state.network_status.ip, sizeof(s_state.network_status.ip), "192.0.2.10");
    (void)snprintf(s_state.network_status.netmask,
                   sizeof(s_state.network_status.netmask),
                   "255.255.255.0");
    (void)snprintf(s_state.network_status.gateway,
                   sizeof(s_state.network_status.gateway),
                   "192.0.2.1");

    s_state.mod_io_status.present = true;
    s_state.mod_io_status.relay_sync = MOD_IO_RELAY_SYNC_SYNCHRONIZED;
    s_state.mod_io_status.relay_mask = 0x00U;
}

void main_startup_stub_set_event_loop_result(esp_err_t result)
{
    s_state.event_loop_result = result;
}

void main_startup_stub_set_device_config_init_result(esp_err_t result)
{
    s_state.device_config_init_result = result;
}

void main_startup_stub_set_board_init_result(esp_err_t result)
{
    s_state.board_init_result = result;
}

void main_startup_stub_set_auth_init_result(esp_err_t result)
{
    s_state.auth_init_result = result;
}

void main_startup_stub_set_relay_init_result(esp_err_t result)
{
    s_state.relay_init_result = result;
}

void main_startup_stub_set_mod_io_init_result(esp_err_t result)
{
    s_state.mod_io_init_result = result;
}

void main_startup_stub_set_input_monitor_start_result(esp_err_t result)
{
    s_state.input_monitor_start_result = result;
}

void main_startup_stub_set_mod_io_probe_result(esp_err_t result)
{
    s_state.mod_io_probe_result = result;
}

void main_startup_stub_set_mod_io_get_status_result(esp_err_t result)
{
    s_state.mod_io_get_status_result = result;
}

void main_startup_stub_set_network_init_result(esp_err_t result)
{
    s_state.network_init_result = result;
}

void main_startup_stub_set_ota_confirm_result(esp_err_t result)
{
    s_state.ota_confirm_result = result;
}

void main_startup_stub_set_network_wait_result(esp_err_t result)
{
    s_state.network_wait_result = result;
}

void main_startup_stub_set_network_register_mdns_result(esp_err_t result)
{
    s_state.network_register_mdns_result = result;
}

void main_startup_stub_set_network_get_status_result(esp_err_t result)
{
    s_state.network_get_status_result = result;
}

void main_startup_stub_set_rest_api_start_result(esp_err_t result)
{
    s_state.rest_api_start_result = result;
}

void main_startup_stub_set_network_status(const network_status_t *status)
{
    if (status != NULL) {
        s_state.network_status = *status;
    }
}

void main_startup_stub_set_mod_io_status(const mod_io_status_t *status)
{
    if (status != NULL) {
        s_state.mod_io_status = *status;
    }
}

size_t main_startup_stub_get_call_count(void)
{
    return s_state.call_count;
}

main_startup_call_t main_startup_stub_get_call(size_t index)
{
    if (index >= s_state.call_count) {
        return MAIN_STARTUP_CALL_REST_API_START;
    }

    return s_state.calls[index];
}

uint32_t main_startup_stub_get_last_wait_timeout_ms(void)
{
    return s_state.last_wait_timeout_ms;
}

uint16_t main_startup_stub_get_last_mdns_port(void)
{
    return s_state.last_mdns_port;
}

const rest_api_config_t *main_startup_stub_get_last_rest_api_config(void)
{
    return s_state.rest_api_config_captured ? &s_state.last_rest_api_config : NULL;
}

esp_err_t esp_event_loop_create_default(void)
{
    main_startup_stub_record_call(MAIN_STARTUP_CALL_EVENT_LOOP_CREATE_DEFAULT);
    return s_state.event_loop_result;
}

esp_err_t device_config_init(void)
{
    main_startup_stub_record_call(MAIN_STARTUP_CALL_DEVICE_CONFIG_INIT);
    return s_state.device_config_init_result;
}

esp_err_t board_init(void)
{
    main_startup_stub_record_call(MAIN_STARTUP_CALL_BOARD_INIT);
    return s_state.board_init_result;
}

i2c_master_bus_handle_t board_i2c_bus_handle(void)
{
    return (i2c_master_bus_handle_t)BOARD_BUS_HANDLE_VALUE;
}

esp_err_t auth_init(void)
{
    main_startup_stub_record_call(MAIN_STARTUP_CALL_AUTH_INIT);
    return s_state.auth_init_result;
}

rest_api_auth_result_t auth_check(httpd_req_t *req, void *ctx)
{
    (void)req;
    (void)ctx;
    return REST_API_AUTH_RESULT_ALLOW;
}

esp_err_t relay_init(void)
{
    main_startup_stub_record_call(MAIN_STARTUP_CALL_RELAY_INIT);
    return s_state.relay_init_result;
}

esp_err_t mod_io_init(i2c_master_bus_handle_t bus_handle)
{
    main_startup_stub_record_call(MAIN_STARTUP_CALL_MOD_IO_INIT);
    if (bus_handle != (i2c_master_bus_handle_t)BOARD_BUS_HANDLE_VALUE) {
        return ESP_ERR_INVALID_ARG;
    }

    return s_state.mod_io_init_result;
}

esp_err_t mod_io_probe(void)
{
    return s_state.mod_io_probe_result;
}

esp_err_t mod_io_get_status(mod_io_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_state.mod_io_get_status_result == ESP_OK) {
        *out = s_state.mod_io_status;
    }

    return s_state.mod_io_get_status_result;
}

esp_err_t input_monitor_start(void)
{
    main_startup_stub_record_call(MAIN_STARTUP_CALL_INPUT_MONITOR_START);
    return s_state.input_monitor_start_result;
}

esp_err_t network_init(void)
{
    main_startup_stub_record_call(MAIN_STARTUP_CALL_NETWORK_INIT);
    return s_state.network_init_result;
}

esp_err_t ota_confirm_running_image_if_pending(void)
{
    main_startup_stub_record_call(MAIN_STARTUP_CALL_OTA_CONFIRM_RUNNING_IMAGE_IF_PENDING);
    return s_state.ota_confirm_result;
}

esp_err_t network_wait_for_ip(uint32_t timeout_ms)
{
    main_startup_stub_record_call(MAIN_STARTUP_CALL_NETWORK_WAIT_FOR_IP);
    s_state.last_wait_timeout_ms = timeout_ms;
    return s_state.network_wait_result;
}

esp_err_t network_register_mdns_service(uint16_t port)
{
    main_startup_stub_record_call(MAIN_STARTUP_CALL_NETWORK_REGISTER_MDNS_SERVICE);
    s_state.last_mdns_port = port;
    return s_state.network_register_mdns_result;
}

esp_err_t network_get_status(network_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_state.network_get_status_result == ESP_OK) {
        *out = s_state.network_status;
    }

    return s_state.network_get_status_result;
}

esp_err_t rest_api_start(const rest_api_config_t *config)
{
    main_startup_stub_record_call(MAIN_STARTUP_CALL_REST_API_START);
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_state.last_rest_api_config = *config;
    s_state.rest_api_config_captured = true;
    return s_state.rest_api_start_result;
}

esp_err_t rest_api_stop(void)
{
    return ESP_OK;
}

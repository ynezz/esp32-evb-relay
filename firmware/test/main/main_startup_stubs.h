#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NETWORK_HOSTNAME_MAX_LEN 63
#define NETWORK_IPV4_ADDR_STR_LEN 16
#define NETWORK_DEFAULT_WAIT_FOR_IP_TIMEOUT_MS 30000U

#define REST_API_DEFAULT_PORT 80U
#define REST_API_HOSTNAME_MAX_LEN 63
#define REST_API_IPV4_ADDR_STR_LEN 16

typedef void *httpd_handle_t;
typedef struct httpd_req httpd_req_t;
typedef void *i2c_master_bus_handle_t;

typedef enum {
    REST_API_AUTH_RESULT_ALLOW = 0,
    REST_API_AUTH_RESULT_UNAUTHORIZED,
    REST_API_AUTH_RESULT_FORBIDDEN,
} rest_api_auth_result_t;

typedef enum {
    REST_API_MODIO_SYNC_ABSENT = 0,
    REST_API_MODIO_SYNC_UNKNOWN,
    REST_API_MODIO_SYNC_SYNCHRONIZED,
} rest_api_modio_sync_t;

typedef enum {
    REST_API_NETWORK_TRANSPORT_NONE = 0,
    REST_API_NETWORK_TRANSPORT_ETHERNET,
    REST_API_NETWORK_TRANSPORT_WIFI,
} rest_api_network_transport_t;

typedef struct {
    bool connected;
    rest_api_network_transport_t transport;
    char hostname[REST_API_HOSTNAME_MAX_LEN + 1];
    char ip[REST_API_IPV4_ADDR_STR_LEN];
    char netmask[REST_API_IPV4_ADDR_STR_LEN];
    char gateway[REST_API_IPV4_ADDR_STR_LEN];
} rest_api_network_status_t;

typedef struct {
    rest_api_network_status_t network;
    bool modio_present;
    rest_api_modio_sync_t modio_sync;
} rest_api_status_view_t;

typedef rest_api_auth_result_t (*rest_api_auth_handler_t)(httpd_req_t *req, void *ctx);
typedef esp_err_t (*rest_api_status_provider_t)(rest_api_status_view_t *status, void *ctx);

typedef struct {
    uint16_t port;
    rest_api_auth_handler_t auth_handler;
    void *auth_ctx;
    rest_api_status_provider_t status_provider;
    void *status_ctx;
} rest_api_config_t;

typedef enum {
    MOD_IO_RELAY_SYNC_ABSENT = 0,
    MOD_IO_RELAY_SYNC_UNKNOWN,
    MOD_IO_RELAY_SYNC_SYNCHRONIZED,
} mod_io_relay_sync_t;

typedef struct {
    bool present;
    mod_io_relay_sync_t relay_sync;
    uint8_t relay_mask;
} mod_io_status_t;

static inline rest_api_modio_sync_t rest_api_modio_sync_from_driver(mod_io_relay_sync_t relay_sync)
{
    switch (relay_sync) {
    case MOD_IO_RELAY_SYNC_ABSENT:
        return REST_API_MODIO_SYNC_ABSENT;
    case MOD_IO_RELAY_SYNC_UNKNOWN:
        return REST_API_MODIO_SYNC_UNKNOWN;
    case MOD_IO_RELAY_SYNC_SYNCHRONIZED:
        return REST_API_MODIO_SYNC_SYNCHRONIZED;
    default:
        return REST_API_MODIO_SYNC_UNKNOWN;
    }
}

typedef enum {
    NETWORK_TRANSPORT_NONE = 0,
    NETWORK_TRANSPORT_ETHERNET,
    NETWORK_TRANSPORT_WIFI,
} network_transport_t;

typedef struct {
    bool connected;
    network_transport_t transport;
    char hostname[NETWORK_HOSTNAME_MAX_LEN + 1];
    char ip[NETWORK_IPV4_ADDR_STR_LEN];
    char netmask[NETWORK_IPV4_ADDR_STR_LEN];
    char gateway[NETWORK_IPV4_ADDR_STR_LEN];
} network_status_t;

typedef enum {
    MAIN_STARTUP_CALL_EVENT_LOOP_CREATE_DEFAULT = 0,
    MAIN_STARTUP_CALL_DEVICE_CONFIG_INIT,
    MAIN_STARTUP_CALL_BOARD_INIT,
    MAIN_STARTUP_CALL_RELAY_INIT,
    MAIN_STARTUP_CALL_MOD_IO_INIT,
    MAIN_STARTUP_CALL_INPUT_MONITOR_START,
    MAIN_STARTUP_CALL_AUTH_INIT,
    MAIN_STARTUP_CALL_NETWORK_INIT,
    MAIN_STARTUP_CALL_OTA_CONFIRM_RUNNING_IMAGE_IF_PENDING,
    MAIN_STARTUP_CALL_NETWORK_WAIT_FOR_IP,
    MAIN_STARTUP_CALL_NETWORK_REGISTER_MDNS_SERVICE,
    MAIN_STARTUP_CALL_REST_API_START,
} main_startup_call_t;

void main_startup_stub_reset(void);

void main_startup_stub_set_event_loop_result(esp_err_t result);
void main_startup_stub_set_device_config_init_result(esp_err_t result);
void main_startup_stub_set_board_init_result(esp_err_t result);
void main_startup_stub_set_auth_init_result(esp_err_t result);
void main_startup_stub_set_relay_init_result(esp_err_t result);
void main_startup_stub_set_mod_io_init_result(esp_err_t result);
void main_startup_stub_set_input_monitor_start_result(esp_err_t result);
void main_startup_stub_set_mod_io_probe_result(esp_err_t result);
void main_startup_stub_set_mod_io_get_status_result(esp_err_t result);
void main_startup_stub_set_network_init_result(esp_err_t result);
void main_startup_stub_set_ota_confirm_result(esp_err_t result);
void main_startup_stub_set_network_wait_result(esp_err_t result);
void main_startup_stub_set_network_register_mdns_result(esp_err_t result);
void main_startup_stub_set_network_get_status_result(esp_err_t result);
void main_startup_stub_set_rest_api_start_result(esp_err_t result);
void main_startup_stub_set_network_status(const network_status_t *status);
void main_startup_stub_set_mod_io_status(const mod_io_status_t *status);

size_t main_startup_stub_get_call_count(void);
main_startup_call_t main_startup_stub_get_call(size_t index);
uint32_t main_startup_stub_get_last_wait_timeout_ms(void);
uint16_t main_startup_stub_get_last_mdns_port(void);
const rest_api_config_t *main_startup_stub_get_last_rest_api_config(void);

esp_err_t esp_event_loop_create_default(void);

esp_err_t device_config_init(void);

esp_err_t board_init(void);
i2c_master_bus_handle_t board_i2c_bus_handle(void);

esp_err_t auth_init(void);
rest_api_auth_result_t auth_check(httpd_req_t *req, void *ctx);

esp_err_t relay_init(void);

esp_err_t mod_io_init(i2c_master_bus_handle_t bus_handle);
esp_err_t mod_io_probe(void);
esp_err_t mod_io_get_status(mod_io_status_t *out);
esp_err_t input_monitor_start(void);

esp_err_t network_init(void);
esp_err_t ota_confirm_running_image_if_pending(void);
esp_err_t network_wait_for_ip(uint32_t timeout_ms);
esp_err_t network_register_mdns_service(uint16_t port);
esp_err_t network_get_status(network_status_t *out);

esp_err_t rest_api_start(const rest_api_config_t *config);
esp_err_t rest_api_stop(void);

#ifdef __cplusplus
}
#endif

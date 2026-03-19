#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "mod_io.h"

#ifdef __cplusplus
extern "C" {
#endif

#define REST_API_DEFAULT_PORT 80U
#define REST_API_HOSTNAME_MAX_LEN 63
#define REST_API_IPV4_ADDR_STR_LEN 16

typedef enum {
    REST_API_AUTH_RESULT_ALLOW = 0,
    REST_API_AUTH_RESULT_UNAUTHORIZED,
    REST_API_AUTH_RESULT_FORBIDDEN,
} rest_api_auth_result_t;

typedef enum {
    REST_API_MODIO_SYNC_ABSENT = 0,
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

static inline rest_api_modio_sync_t rest_api_modio_sync_from_driver(mod_io_relay_sync_t relay_sync)
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

static inline const char *rest_api_network_transport_to_string(rest_api_network_transport_t transport)
{
    switch (transport) {
    case REST_API_NETWORK_TRANSPORT_ETHERNET:
        return "ethernet";
    case REST_API_NETWORK_TRANSPORT_WIFI:
        return "wifi";
    case REST_API_NETWORK_TRANSPORT_NONE:
    default:
        return "none";
    }
}

esp_err_t rest_api_start(const rest_api_config_t *config);
esp_err_t rest_api_stop(void);

httpd_handle_t rest_api_get_server(void);

bool rest_api_parse_id_from_uri(const char *uri, const char *prefix, uint32_t *out_id);
const char *rest_api_modio_sync_to_string(rest_api_modio_sync_t sync_state);
esp_err_t rest_api_send_error(httpd_req_t *req,
                              int http_status,
                              const char *code,
                              const char *message,
                              bool authenticated);

#if defined(REST_API_ENABLE_TESTING_API)
void rest_api_sse_hold_dispatch_task_on_shutdown_for_testing(bool hold);
bool rest_api_sse_wait_for_dispatch_shutdown_reached_for_testing(uint32_t timeout_ms);
bool rest_api_sse_dispatch_task_deleted_by_stop_for_testing(void);
void rest_api_sse_force_next_client_task_create_failure_for_testing(void);
#endif

#ifdef __cplusplus
}
#endif

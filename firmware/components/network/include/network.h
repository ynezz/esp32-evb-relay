#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NETWORK_HOSTNAME_MAX_LEN 63
#define NETWORK_IPV4_ADDR_STR_LEN 16
#define NETWORK_DEFAULT_WAIT_FOR_IP_TIMEOUT_MS 30000U

typedef struct {
    bool connected;
    char hostname[NETWORK_HOSTNAME_MAX_LEN + 1];
    char ip[NETWORK_IPV4_ADDR_STR_LEN];
    char netmask[NETWORK_IPV4_ADDR_STR_LEN];
    char gateway[NETWORK_IPV4_ADDR_STR_LEN];
} network_status_t;

esp_err_t network_init(void);
esp_err_t network_wait_for_ip(uint32_t timeout_ms);
esp_err_t network_register_mdns_service(uint16_t port);
esp_err_t network_get_status(network_status_t *out);

#ifdef __cplusplus
}
#endif

#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "esp_netif_ip_addr.h"

typedef struct esp_netif_obj esp_netif_t;

typedef struct {
    int dummy;
} esp_netif_inherent_config_t;

typedef struct {
    const esp_netif_inherent_config_t *base;
    void *stack;
} esp_netif_config_t;

typedef struct {
    esp_ip4_addr_t ip;
    esp_ip4_addr_t netmask;
    esp_ip4_addr_t gw;
} esp_netif_ip_info_t;

typedef struct {
    esp_netif_t *esp_netif;
    esp_netif_ip_info_t ip_info;
} ip_event_got_ip_t;

esp_err_t esp_netif_init(void);
esp_netif_t *esp_netif_new(const esp_netif_config_t *config);
void esp_netif_destroy(esp_netif_t *netif);
esp_err_t esp_netif_attach(esp_netif_t *netif, void *glue_handle);
esp_err_t esp_netif_set_hostname(esp_netif_t *netif, const char *hostname);

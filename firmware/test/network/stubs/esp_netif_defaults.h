#pragma once

#include "esp_netif.h"

#define ESP_NETIF_DEFAULT_ETH() ((esp_netif_config_t){0})
#define ESP_NETIF_INHERENT_DEFAULT_WIFI_STA() ((esp_netif_inherent_config_t){0})
#define ESP_NETIF_NETSTACK_DEFAULT_WIFI_STA NULL

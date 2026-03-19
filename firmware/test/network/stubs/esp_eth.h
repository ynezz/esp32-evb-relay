#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef struct esp_eth_handle_obj *esp_eth_handle_t;
typedef void *esp_eth_netif_glue_handle_t;

typedef struct esp_eth_mac_s {
    esp_err_t (*del)(struct esp_eth_mac_s *self);
} esp_eth_mac_t;

typedef struct esp_eth_phy_s {
    esp_err_t (*del)(struct esp_eth_phy_s *self);
} esp_eth_phy_t;

typedef struct {
    int dummy;
} eth_mac_config_t;

typedef struct {
    int phy_addr;
    int reset_gpio_num;
} eth_phy_config_t;

typedef struct {
    struct {
        int mdc_num;
        int mdio_num;
    } smi_gpio;
} eth_esp32_emac_config_t;

typedef struct {
    esp_eth_mac_t *mac;
    esp_eth_phy_t *phy;
} esp_eth_config_t;

#define ETH_MAC_DEFAULT_CONFIG() ((eth_mac_config_t){0})
#define ETH_PHY_DEFAULT_CONFIG() ((eth_phy_config_t){0})
#define ETH_ESP32_EMAC_DEFAULT_CONFIG() ((eth_esp32_emac_config_t){0})
#define ETH_DEFAULT_CONFIG(mac_handle, phy_handle)                                         \
    ((esp_eth_config_t){.mac = (mac_handle), .phy = (phy_handle)})

#define ETH_CMD_G_MAC_ADDR 1

#define ETHERNET_EVENT_START 1
#define ETHERNET_EVENT_CONNECTED 2
#define ETHERNET_EVENT_DISCONNECTED 3
#define ETHERNET_EVENT_STOP 4

esp_eth_mac_t *esp_eth_mac_new_esp32(const eth_esp32_emac_config_t *config,
                                     const eth_mac_config_t *mac_config);
esp_eth_phy_t *esp_eth_phy_new_lan87xx(const eth_phy_config_t *config);
esp_err_t esp_eth_driver_install(const esp_eth_config_t *config, esp_eth_handle_t *out_handle);
esp_eth_netif_glue_handle_t esp_eth_new_netif_glue(esp_eth_handle_t handle);
esp_err_t esp_eth_del_netif_glue(esp_eth_netif_glue_handle_t glue);
esp_err_t esp_eth_driver_uninstall(esp_eth_handle_t handle);
esp_err_t esp_eth_start(esp_eth_handle_t handle);
esp_err_t esp_eth_stop(esp_eth_handle_t handle);
esp_err_t esp_eth_ioctl(esp_eth_handle_t handle, int cmd, void *data);

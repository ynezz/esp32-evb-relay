#include "network.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "device_config.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "mdns.h"

static const char *TAG = "network";

#define NETWORK_EVENT_GOT_IP BIT0
#define NETWORK_DHCP_HOSTNAME_MAX_LEN 32U

typedef struct {
    bool initialized;
    bool mdns_registered;
    EventGroupHandle_t event_group;
    SemaphoreHandle_t mutex;
    esp_netif_t *netif;
    esp_eth_handle_t eth_handle;
    esp_eth_mac_t *mac;
    esp_eth_phy_t *phy;
    esp_eth_netif_glue_handle_t netif_glue;
    network_status_t status;
} network_state_t;

static network_state_t s_state;

static void network_clear_ipv4_fields(network_status_t *status)
{
    status->connected = false;
    status->ip[0] = '\0';
    status->netmask[0] = '\0';
    status->gateway[0] = '\0';
}

static void network_update_disconnected_status(void)
{
    if ((s_state.mutex != NULL) && (xSemaphoreTake(s_state.mutex, portMAX_DELAY) == pdTRUE)) {
        network_clear_ipv4_fields(&s_state.status);
        xSemaphoreGive(s_state.mutex);
    }

    if (s_state.event_group != NULL) {
        xEventGroupClearBits(s_state.event_group, NETWORK_EVENT_GOT_IP);
    }
}

static void network_copy_ipv4_address(char *buffer, size_t buffer_size, const esp_ip4_addr_t *address)
{
    if ((buffer == NULL) || (buffer_size == 0U) || (address == NULL)) {
        return;
    }

    (void)snprintf(buffer, buffer_size, IPSTR, IP2STR(address));
}

static void network_eth_event_handler(void *arg,
                                      esp_event_base_t event_base,
                                      int32_t event_id,
                                      void *event_data)
{
    uint8_t mac_addr[6] = {0};
    esp_eth_handle_t eth_handle = NULL;

    (void)arg;
    (void)event_base;

    if (event_data != NULL) {
        eth_handle = *(esp_eth_handle_t *)event_data;
    }

    switch (event_id) {
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet started");
        break;
    case ETHERNET_EVENT_CONNECTED:
        if ((eth_handle != NULL) &&
                (esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr) == ESP_OK)) {
            ESP_LOGI(TAG,
                     "Ethernet link up, MAC=%02x:%02x:%02x:%02x:%02x:%02x",
                     mac_addr[0],
                     mac_addr[1],
                     mac_addr[2],
                     mac_addr[3],
                     mac_addr[4],
                     mac_addr[5]);
        } else {
            ESP_LOGI(TAG, "Ethernet link up");
        }
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        network_update_disconnected_status();
        ESP_LOGW(TAG, "Ethernet link down");
        break;
    case ETHERNET_EVENT_STOP:
        network_update_disconnected_status();
        ESP_LOGW(TAG, "Ethernet stopped");
        break;
    default:
        break;
    }
}

static void network_got_ip_event_handler(void *arg,
                                         esp_event_base_t event_base,
                                         int32_t event_id,
                                         void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

    (void)arg;
    (void)event_base;
    (void)event_id;

    if ((event == NULL) || (event->esp_netif != s_state.netif) || (s_state.mutex == NULL)) {
        return;
    }

    if (xSemaphoreTake(s_state.mutex, portMAX_DELAY) == pdTRUE) {
        s_state.status.connected = true;
        network_copy_ipv4_address(s_state.status.ip, sizeof(s_state.status.ip), &event->ip_info.ip);
        network_copy_ipv4_address(s_state.status.netmask,
                                  sizeof(s_state.status.netmask),
                                  &event->ip_info.netmask);
        network_copy_ipv4_address(s_state.status.gateway,
                                  sizeof(s_state.status.gateway),
                                  &event->ip_info.gw);
        xSemaphoreGive(s_state.mutex);
    }

    if (s_state.event_group != NULL) {
        xEventGroupSetBits(s_state.event_group, NETWORK_EVENT_GOT_IP);
    }

    ESP_LOGI(TAG,
             "Ethernet got IP: ip=" IPSTR " netmask=" IPSTR " gateway=" IPSTR,
             IP2STR(&event->ip_info.ip),
             IP2STR(&event->ip_info.netmask),
             IP2STR(&event->ip_info.gw));
}

esp_err_t network_init(void)
{
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    eth_esp32_emac_config_t esp32_emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    char hostname[NETWORK_HOSTNAME_MAX_LEN + 1];
    esp_err_t ret;

    if (s_state.initialized) {
        return ESP_OK;
    }

    memset(&s_state, 0, sizeof(s_state));
    ret = device_config_get_hostname(hostname, sizeof(hostname));
    if (ret != ESP_OK) {
        return ret;
    }

    s_state.event_group = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_state.event_group != NULL, ESP_ERR_NO_MEM, TAG, "Failed to create event group");

    s_state.mutex = xSemaphoreCreateMutex();
    if (s_state.mutex == NULL) {
        vEventGroupDelete(s_state.event_group);
        s_state.event_group = NULL;
        return ESP_ERR_NO_MEM;
    }

    (void)snprintf(s_state.status.hostname, sizeof(s_state.status.hostname), "%s", hostname);
    network_clear_ipv4_fields(&s_state.status);

    ret = esp_netif_init();
    if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE)) {
        goto err_cleanup;
    }

    s_state.netif = esp_netif_new(&netif_config);
    ESP_GOTO_ON_FALSE(s_state.netif != NULL, ESP_ERR_NO_MEM, err_cleanup, TAG, "Failed to create Ethernet netif");

    if (strlen(hostname) <= NETWORK_DHCP_HOSTNAME_MAX_LEN) {
        ESP_GOTO_ON_ERROR(esp_netif_set_hostname(s_state.netif, hostname),
                          err_cleanup,
                          TAG,
                          "Failed to set Ethernet hostname");
    } else {
        ESP_LOGW(TAG,
                 "Skipping DHCP hostname update because \"%s\" exceeds ESP-IDF's %u-character limit",
                 hostname,
                 (unsigned)NETWORK_DHCP_HOSTNAME_MAX_LEN);
    }

    phy_config.phy_addr = 0;
    phy_config.reset_gpio_num = -1;
    esp32_emac_config.smi_gpio.mdc_num = BOARD_ETH_MDC;
    esp32_emac_config.smi_gpio.mdio_num = BOARD_ETH_MDIO;

    s_state.mac = esp_eth_mac_new_esp32(&esp32_emac_config, &mac_config);
    ESP_GOTO_ON_FALSE(s_state.mac != NULL, ESP_ERR_NO_MEM, err_cleanup, TAG, "Failed to create Ethernet MAC");

    s_state.phy = esp_eth_phy_new_lan87xx(&phy_config);
    ESP_GOTO_ON_FALSE(s_state.phy != NULL, ESP_ERR_NO_MEM, err_cleanup, TAG, "Failed to create Ethernet PHY");

    {
        esp_eth_config_t config = ETH_DEFAULT_CONFIG(s_state.mac, s_state.phy);
        ESP_GOTO_ON_ERROR(esp_eth_driver_install(&config, &s_state.eth_handle),
                          err_cleanup,
                          TAG,
                          "Failed to install Ethernet driver");
    }

    s_state.netif_glue = esp_eth_new_netif_glue(s_state.eth_handle);
    ESP_GOTO_ON_FALSE(s_state.netif_glue != NULL,
                      ESP_ERR_NO_MEM,
                      err_cleanup,
                      TAG,
                      "Failed to create Ethernet netif glue");

    ESP_GOTO_ON_ERROR(esp_netif_attach(s_state.netif, s_state.netif_glue),
                      err_cleanup,
                      TAG,
                      "Failed to attach Ethernet netif");
    ESP_GOTO_ON_ERROR(esp_event_handler_register(ETH_EVENT,
                                                 ESP_EVENT_ANY_ID,
                                                 network_eth_event_handler,
                                                 NULL),
                      err_cleanup,
                      TAG,
                      "Failed to register Ethernet event handler");
    ESP_GOTO_ON_ERROR(esp_event_handler_register(IP_EVENT,
                                                 IP_EVENT_ETH_GOT_IP,
                                                 network_got_ip_event_handler,
                                                 NULL),
                      err_cleanup,
                      TAG,
                      "Failed to register Ethernet IP event handler");
    ESP_GOTO_ON_ERROR(esp_eth_start(s_state.eth_handle),
                      err_cleanup,
                      TAG,
                      "Failed to start Ethernet");

    s_state.initialized = true;
    return ESP_OK;

err_cleanup:
    if (s_state.netif_glue != NULL) {
        (void)esp_eth_del_netif_glue(s_state.netif_glue);
        s_state.netif_glue = NULL;
    }

    if (s_state.eth_handle != NULL) {
        (void)esp_eth_driver_uninstall(s_state.eth_handle);
        s_state.eth_handle = NULL;
    }

    if (s_state.phy != NULL) {
        (void)s_state.phy->del(s_state.phy);
        s_state.phy = NULL;
    }

    if (s_state.mac != NULL) {
        (void)s_state.mac->del(s_state.mac);
        s_state.mac = NULL;
    }

    if (s_state.netif != NULL) {
        esp_netif_destroy(s_state.netif);
        s_state.netif = NULL;
    }

    if (s_state.mutex != NULL) {
        vSemaphoreDelete(s_state.mutex);
        s_state.mutex = NULL;
    }

    if (s_state.event_group != NULL) {
        vEventGroupDelete(s_state.event_group);
        s_state.event_group = NULL;
    }

    memset(&s_state.status, 0, sizeof(s_state.status));
    return ret;
}

esp_err_t network_wait_for_ip(uint32_t timeout_ms)
{
    EventBits_t bits;

    ESP_RETURN_ON_FALSE(s_state.initialized, ESP_ERR_INVALID_STATE, TAG, "Network is not initialized");
    ESP_RETURN_ON_FALSE(s_state.event_group != NULL, ESP_ERR_INVALID_STATE, TAG, "Network event group is missing");

    bits = xEventGroupWaitBits(s_state.event_group,
                               NETWORK_EVENT_GOT_IP,
                               pdFALSE,
                               pdTRUE,
                               pdMS_TO_TICKS(timeout_ms));
    if ((bits & NETWORK_EVENT_GOT_IP) == 0U) {
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t network_register_mdns_service(uint16_t port)
{
    static const char *const BOARD_TYPE = "esp32-evb";
    network_status_t status;
    const esp_app_desc_t *app_desc;
    mdns_txt_item_t service_txt[2];

    ESP_RETURN_ON_FALSE(s_state.initialized, ESP_ERR_INVALID_STATE, TAG, "Network is not initialized");
    ESP_RETURN_ON_FALSE(port != 0U, ESP_ERR_INVALID_ARG, TAG, "mDNS service port is required");

    if (s_state.mdns_registered) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(network_get_status(&status), TAG, "Failed to load network status");
    ESP_RETURN_ON_FALSE(status.connected, ESP_ERR_INVALID_STATE, TAG, "Ethernet must have an IP before mDNS");

    app_desc = esp_app_get_description();
    service_txt[0].key = "fw_version";
    service_txt[0].value = (app_desc != NULL) ? app_desc->version : "unknown";
    service_txt[1].key = "board";
    service_txt[1].value = BOARD_TYPE;

    ESP_RETURN_ON_ERROR(mdns_init(), TAG, "Failed to initialize mDNS");
    ESP_RETURN_ON_ERROR(mdns_hostname_set(status.hostname), TAG, "Failed to set mDNS hostname");
    ESP_RETURN_ON_ERROR(mdns_instance_name_set("ESP32-EVB Relay"), TAG,
                        "Failed to set mDNS instance name");
    ESP_RETURN_ON_ERROR(mdns_service_add(status.hostname,
                                         "_http",
                                         "_tcp",
                                         port,
                                         service_txt,
                                         sizeof(service_txt) / sizeof(service_txt[0])),
                        TAG,
                        "Failed to register mDNS HTTP service");

    s_state.mdns_registered = true;
    return ESP_OK;
}

esp_err_t network_get_status(network_status_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "Status output buffer is required");
    ESP_RETURN_ON_FALSE(s_state.initialized, ESP_ERR_INVALID_STATE, TAG, "Network is not initialized");
    ESP_RETURN_ON_FALSE(s_state.mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "Network mutex is missing");

    if (xSemaphoreTake(s_state.mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    *out = s_state.status;
    xSemaphoreGive(s_state.mutex);
    return ESP_OK;
}

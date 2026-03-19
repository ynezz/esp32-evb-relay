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
#include "esp_netif_defaults.h"
#include "esp_netif_ip_addr.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "mdns.h"

static const char *TAG = "network";

#define NETWORK_EVENT_ETH_GOT_IP BIT0
#define NETWORK_EVENT_WIFI_GOT_IP BIT1
#define NETWORK_DHCP_HOSTNAME_MAX_LEN 32U

typedef struct {
    bool connected;
    char ip[NETWORK_IPV4_ADDR_STR_LEN];
    char netmask[NETWORK_IPV4_ADDR_STR_LEN];
    char gateway[NETWORK_IPV4_ADDR_STR_LEN];
} network_interface_status_t;

typedef struct {
    bool initialized;
    bool mdns_registered;
    EventGroupHandle_t event_group;
    SemaphoreHandle_t mutex;
    device_config_network_policy_t policy;
    device_config_wifi_sta_credentials_t wifi_credentials;
    network_transport_t active_transport;
    esp_netif_t *eth_netif;
    esp_eth_handle_t eth_handle;
    esp_eth_mac_t *mac;
    esp_eth_phy_t *phy;
    esp_eth_netif_glue_handle_t netif_glue;
    esp_netif_t *wifi_netif;
    bool wifi_default_handlers_set;
    bool wifi_initialized;
    bool wifi_started;
    network_interface_status_t ethernet_status;
    network_interface_status_t wifi_status;
    network_status_t status;
} network_state_t;

static network_state_t s_state;

static void network_clear_interface_status(network_interface_status_t *status)
{
    if (status == NULL) {
        return;
    }

    status->connected = false;
    status->ip[0] = '\0';
    status->netmask[0] = '\0';
    status->gateway[0] = '\0';
}

static void network_clear_public_ipv4_fields(network_status_t *status)
{
    if (status == NULL) {
        return;
    }

    status->connected = false;
    status->ip[0] = '\0';
    status->netmask[0] = '\0';
    status->gateway[0] = '\0';
}

static void network_copy_ipv4_address(char *buffer, size_t buffer_size, const esp_ip4_addr_t *address)
{
    if ((buffer == NULL) || (buffer_size == 0U) || (address == NULL)) {
        return;
    }

    (void)snprintf(buffer, buffer_size, IPSTR, IP2STR(address));
}

static void network_sync_public_status_locked(void)
{
    const network_interface_status_t *source = NULL;

    switch (s_state.active_transport) {
    case NETWORK_TRANSPORT_ETHERNET:
        source = &s_state.ethernet_status;
        break;
    case NETWORK_TRANSPORT_WIFI:
        source = &s_state.wifi_status;
        break;
    case NETWORK_TRANSPORT_NONE:
    default:
        break;
    }

    s_state.status.transport = s_state.active_transport;
    if (source == NULL) {
        network_clear_public_ipv4_fields(&s_state.status);
        return;
    }

    s_state.status.connected = source->connected;
    memcpy(s_state.status.ip, source->ip, sizeof(s_state.status.ip));
    memcpy(s_state.status.netmask, source->netmask, sizeof(s_state.status.netmask));
    memcpy(s_state.status.gateway, source->gateway, sizeof(s_state.status.gateway));
}

static void network_set_active_transport(network_transport_t transport)
{
    if ((s_state.mutex == NULL) || (xSemaphoreTake(s_state.mutex, portMAX_DELAY) != pdTRUE)) {
        return;
    }

    s_state.active_transport = transport;
    network_sync_public_status_locked();
    xSemaphoreGive(s_state.mutex);
}

static void network_mark_transport_disconnected(network_transport_t transport)
{
    EventBits_t bit = 0U;

    if (transport == NETWORK_TRANSPORT_ETHERNET) {
        bit = NETWORK_EVENT_ETH_GOT_IP;
    } else if (transport == NETWORK_TRANSPORT_WIFI) {
        bit = NETWORK_EVENT_WIFI_GOT_IP;
    }

    if ((s_state.mutex != NULL) && (xSemaphoreTake(s_state.mutex, portMAX_DELAY) == pdTRUE)) {
        if (transport == NETWORK_TRANSPORT_ETHERNET) {
            network_clear_interface_status(&s_state.ethernet_status);
        } else if (transport == NETWORK_TRANSPORT_WIFI) {
            network_clear_interface_status(&s_state.wifi_status);
        }
        if (s_state.active_transport == transport) {
            network_sync_public_status_locked();
        }
        xSemaphoreGive(s_state.mutex);
    }

    if ((bit != 0U) && (s_state.event_group != NULL)) {
        xEventGroupClearBits(s_state.event_group, bit);
    }
}

static void network_cache_ip_event(network_transport_t transport, const ip_event_got_ip_t *event)
{
    EventBits_t bit;

    if ((event == NULL) || (s_state.mutex == NULL)) {
        return;
    }

    if (transport == NETWORK_TRANSPORT_ETHERNET) {
        bit = NETWORK_EVENT_ETH_GOT_IP;
    } else {
        bit = NETWORK_EVENT_WIFI_GOT_IP;
    }

    if (xSemaphoreTake(s_state.mutex, portMAX_DELAY) == pdTRUE) {
        network_interface_status_t *target =
            (transport == NETWORK_TRANSPORT_ETHERNET) ? &s_state.ethernet_status : &s_state.wifi_status;

        target->connected = true;
        network_copy_ipv4_address(target->ip, sizeof(target->ip), &event->ip_info.ip);
        network_copy_ipv4_address(target->netmask, sizeof(target->netmask), &event->ip_info.netmask);
        network_copy_ipv4_address(target->gateway, sizeof(target->gateway), &event->ip_info.gw);
        if (s_state.active_transport == transport) {
            network_sync_public_status_locked();
        }
        xSemaphoreGive(s_state.mutex);
    }

    if (s_state.event_group != NULL) {
        xEventGroupSetBits(s_state.event_group, bit);
    }
}

static const char *network_transport_name(network_transport_t transport)
{
    switch (transport) {
    case NETWORK_TRANSPORT_ETHERNET:
        return "Ethernet";
    case NETWORK_TRANSPORT_WIFI:
        return "WiFi";
    case NETWORK_TRANSPORT_NONE:
    default:
        return "none";
    }
}

static uint32_t network_prefer_ethernet_wait_ms(uint32_t timeout_ms)
{
    if (timeout_ms == 0U) {
        return 0U;
    }

    if (timeout_ms > NETWORK_PREFER_ETHERNET_DELAY_MS) {
        return NETWORK_PREFER_ETHERNET_DELAY_MS;
    }

    if (timeout_ms == 1U) {
        return 1U;
    }

    return timeout_ms / 2U;
}

static esp_err_t network_start_ethernet(void)
{
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    eth_esp32_emac_config_t esp32_emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    esp_err_t ret;

    if (s_state.eth_handle != NULL) {
        return ESP_OK;
    }

    s_state.eth_netif = esp_netif_new(&netif_config);
    ESP_RETURN_ON_FALSE(s_state.eth_netif != NULL, ESP_ERR_NO_MEM, TAG, "Failed to create Ethernet netif");

    if (strlen(s_state.status.hostname) <= NETWORK_DHCP_HOSTNAME_MAX_LEN) {
        ret = esp_netif_set_hostname(s_state.eth_netif, s_state.status.hostname);
        if (ret != ESP_OK) {
            esp_netif_destroy(s_state.eth_netif);
            s_state.eth_netif = NULL;
            return ret;
        }
    } else {
        ESP_LOGW(TAG,
                 "Skipping Ethernet DHCP hostname update because \"%s\" exceeds ESP-IDF's %u-character limit",
                 s_state.status.hostname,
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

    ESP_GOTO_ON_ERROR(esp_netif_attach(s_state.eth_netif, s_state.netif_glue),
                      err_cleanup,
                      TAG,
                      "Failed to attach Ethernet netif");
    ESP_GOTO_ON_ERROR(esp_eth_start(s_state.eth_handle),
                      err_cleanup,
                      TAG,
                      "Failed to start Ethernet");

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
    if (s_state.eth_netif != NULL) {
        esp_netif_destroy(s_state.eth_netif);
        s_state.eth_netif = NULL;
    }
    return ret;
}

static void network_stop_ethernet(void)
{
    if (s_state.eth_handle != NULL) {
        (void)esp_eth_stop(s_state.eth_handle);
    }

    network_mark_transport_disconnected(NETWORK_TRANSPORT_ETHERNET);

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
    if (s_state.eth_netif != NULL) {
        esp_netif_destroy(s_state.eth_netif);
        s_state.eth_netif = NULL;
    }
}

static esp_err_t network_start_wifi(void)
{
    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    esp_netif_inherent_config_t netif_inherent_config = ESP_NETIF_INHERENT_DEFAULT_WIFI_STA();
    esp_netif_config_t netif_config = {
        .base = &netif_inherent_config,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_WIFI_STA,
    };
    wifi_config_t wifi_config = {0};
    esp_err_t err;

    ESP_RETURN_ON_FALSE(s_state.wifi_credentials.ssid_set,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "WiFi policy requires configured STA credentials");

    if (!s_state.wifi_initialized) {
        s_state.wifi_netif = esp_netif_new(&netif_config);
        ESP_RETURN_ON_FALSE(s_state.wifi_netif != NULL, ESP_ERR_NO_MEM, TAG, "Failed to create WiFi STA netif");

        err = esp_netif_attach_wifi_station(s_state.wifi_netif);
        if (err != ESP_OK) {
            esp_netif_destroy(s_state.wifi_netif);
            s_state.wifi_netif = NULL;
            return err;
        }

        err = esp_wifi_set_default_wifi_sta_handlers();
        if (err != ESP_OK) {
            (void)esp_wifi_clear_default_wifi_driver_and_handlers(s_state.wifi_netif);
            esp_netif_destroy(s_state.wifi_netif);
            s_state.wifi_netif = NULL;
            return err;
        }
        s_state.wifi_default_handlers_set = true;

        err = esp_wifi_init(&wifi_init_config);
        if (err != ESP_OK) {
            (void)esp_wifi_clear_default_wifi_driver_and_handlers(s_state.wifi_netif);
            s_state.wifi_default_handlers_set = false;
            esp_netif_destroy(s_state.wifi_netif);
            s_state.wifi_netif = NULL;
            return err;
        }

        s_state.wifi_initialized = true;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "Failed to use RAM-backed WiFi storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "Failed to set WiFi STA mode");

    memcpy(wifi_config.sta.ssid,
           s_state.wifi_credentials.ssid,
           strnlen(s_state.wifi_credentials.ssid, sizeof(wifi_config.sta.ssid)));
    memcpy(wifi_config.sta.password,
           s_state.wifi_credentials.passphrase,
           strnlen(s_state.wifi_credentials.passphrase, sizeof(wifi_config.sta.password)));
    wifi_config.sta.threshold.authmode =
        s_state.wifi_credentials.passphrase_set ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "Failed to set WiFi STA config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Failed to start WiFi STA");
    s_state.wifi_started = true;
    ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "Failed to start WiFi STA connection");
    return ESP_OK;
}

static void network_stop_wifi(void)
{
    if (s_state.wifi_started) {
        s_state.wifi_started = false;
        (void)esp_wifi_stop();
    }

    network_mark_transport_disconnected(NETWORK_TRANSPORT_WIFI);

    if (s_state.wifi_initialized) {
        (void)esp_wifi_deinit();
        s_state.wifi_initialized = false;
    }

    if ((s_state.wifi_netif != NULL) && s_state.wifi_default_handlers_set) {
        (void)esp_wifi_clear_default_wifi_driver_and_handlers(s_state.wifi_netif);
        s_state.wifi_default_handlers_set = false;
    }

    if (s_state.wifi_netif != NULL) {
        esp_netif_destroy(s_state.wifi_netif);
        s_state.wifi_netif = NULL;
    }
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
        network_mark_transport_disconnected(NETWORK_TRANSPORT_ETHERNET);
        ESP_LOGW(TAG, "Ethernet link down");
        break;
    case ETHERNET_EVENT_STOP:
        network_mark_transport_disconnected(NETWORK_TRANSPORT_ETHERNET);
        ESP_LOGW(TAG, "Ethernet stopped");
        break;
    default:
        break;
    }
}

static void network_eth_got_ip_event_handler(void *arg,
                                             esp_event_base_t event_base,
                                             int32_t event_id,
                                             void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

    (void)arg;
    (void)event_base;
    (void)event_id;

    if ((event == NULL) || (event->esp_netif != s_state.eth_netif)) {
        return;
    }

    network_cache_ip_event(NETWORK_TRANSPORT_ETHERNET, event);
    ESP_LOGI(TAG,
             "Ethernet got IP: ip=" IPSTR " netmask=" IPSTR " gateway=" IPSTR,
             IP2STR(&event->ip_info.ip),
             IP2STR(&event->ip_info.netmask),
             IP2STR(&event->ip_info.gw));
}

static void network_wifi_event_handler(void *arg,
                                       esp_event_base_t event_base,
                                       int32_t event_id,
                                       void *event_data)
{
    (void)arg;
    (void)event_base;

    switch (event_id) {
    case WIFI_EVENT_STA_START:
        ESP_LOGI(TAG, "WiFi STA started");
        break;
    case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGI(TAG, "WiFi STA associated");
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        network_mark_transport_disconnected(NETWORK_TRANSPORT_WIFI);
        if (s_state.wifi_started) {
            const wifi_event_sta_disconnected_t *event =
                (const wifi_event_sta_disconnected_t *)event_data;

            if (event != NULL) {
                ESP_LOGW(TAG, "WiFi STA disconnect reason=%u, reconnecting", (unsigned)event->reason);
            } else {
                ESP_LOGW(TAG, "WiFi STA disconnected, reconnecting");
            }
            (void)esp_wifi_connect();
        }
        break;
    case WIFI_EVENT_STA_STOP:
        network_mark_transport_disconnected(NETWORK_TRANSPORT_WIFI);
        ESP_LOGW(TAG, "WiFi STA stopped");
        break;
    default:
        break;
    }
}

static void network_wifi_got_ip_event_handler(void *arg,
                                              esp_event_base_t event_base,
                                              int32_t event_id,
                                              void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

    (void)arg;
    (void)event_base;
    (void)event_id;

    if ((event == NULL) || (event->esp_netif != s_state.wifi_netif)) {
        return;
    }

    network_cache_ip_event(NETWORK_TRANSPORT_WIFI, event);
    ESP_LOGI(TAG,
             "WiFi got IP: ip=" IPSTR " netmask=" IPSTR " gateway=" IPSTR,
             IP2STR(&event->ip_info.ip),
             IP2STR(&event->ip_info.netmask),
             IP2STR(&event->ip_info.gw));
}

esp_err_t network_init(void)
{
    esp_err_t ret;

    if (s_state.initialized) {
        return ESP_OK;
    }

    memset(&s_state, 0, sizeof(s_state));

    ret = device_config_get_hostname(s_state.status.hostname, sizeof(s_state.status.hostname));
    if (ret != ESP_OK) {
        return ret;
    }

    ret = device_config_get_wifi_sta_credentials(&s_state.wifi_credentials);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = device_config_get_network_policy(&s_state.policy);
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

    s_state.status.transport = NETWORK_TRANSPORT_NONE;
    network_clear_public_ipv4_fields(&s_state.status);
    network_clear_interface_status(&s_state.ethernet_status);
    network_clear_interface_status(&s_state.wifi_status);
    s_state.initialized = true;

    ret = esp_netif_init();
    if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE)) {
        goto err_cleanup;
    }

    ESP_GOTO_ON_ERROR(esp_event_handler_register(ETH_EVENT,
                                                 ESP_EVENT_ANY_ID,
                                                 network_eth_event_handler,
                                                 NULL),
                      err_cleanup,
                      TAG,
                      "Failed to register Ethernet event handler");
    ESP_GOTO_ON_ERROR(esp_event_handler_register(IP_EVENT,
                                                 IP_EVENT_ETH_GOT_IP,
                                                 network_eth_got_ip_event_handler,
                                                 NULL),
                      err_cleanup,
                      TAG,
                      "Failed to register Ethernet IP event handler");
    ESP_GOTO_ON_ERROR(esp_event_handler_register(WIFI_EVENT,
                                                 ESP_EVENT_ANY_ID,
                                                 network_wifi_event_handler,
                                                 NULL),
                      err_cleanup,
                      TAG,
                      "Failed to register WiFi event handler");
    ESP_GOTO_ON_ERROR(esp_event_handler_register(IP_EVENT,
                                                 IP_EVENT_STA_GOT_IP,
                                                 network_wifi_got_ip_event_handler,
                                                 NULL),
                      err_cleanup,
                      TAG,
                      "Failed to register WiFi IP event handler");

    switch (s_state.policy) {
    case DEVICE_CONFIG_NETWORK_POLICY_ETHERNET_ONLY:
    case DEVICE_CONFIG_NETWORK_POLICY_PREFER_ETHERNET:
        network_set_active_transport(NETWORK_TRANSPORT_ETHERNET);
        ESP_GOTO_ON_ERROR(network_start_ethernet(), err_cleanup, TAG, "Failed to start Ethernet");
        break;
    case DEVICE_CONFIG_NETWORK_POLICY_WIFI_ONLY:
        network_set_active_transport(NETWORK_TRANSPORT_WIFI);
        ESP_GOTO_ON_ERROR(network_start_wifi(), err_cleanup, TAG, "Failed to start WiFi STA");
        break;
    default:
        ret = ESP_ERR_INVALID_STATE;
        goto err_cleanup;
    }

    return ESP_OK;

err_cleanup:
    network_stop_wifi();
    network_stop_ethernet();
    if (s_state.mutex != NULL) {
        vSemaphoreDelete(s_state.mutex);
        s_state.mutex = NULL;
    }
    if (s_state.event_group != NULL) {
        vEventGroupDelete(s_state.event_group);
        s_state.event_group = NULL;
    }
    memset(&s_state, 0, sizeof(s_state));
    return ret;
}

esp_err_t network_wait_for_ip(uint32_t timeout_ms)
{
    EventBits_t bits;

    ESP_RETURN_ON_FALSE(s_state.initialized, ESP_ERR_INVALID_STATE, TAG, "Network is not initialized");
    ESP_RETURN_ON_FALSE(s_state.event_group != NULL, ESP_ERR_INVALID_STATE, TAG, "Network event group is missing");

    switch (s_state.policy) {
    case DEVICE_CONFIG_NETWORK_POLICY_ETHERNET_ONLY:
        bits = xEventGroupWaitBits(s_state.event_group,
                                   NETWORK_EVENT_ETH_GOT_IP,
                                   pdFALSE,
                                   pdTRUE,
                                   pdMS_TO_TICKS(timeout_ms));
        return ((bits & NETWORK_EVENT_ETH_GOT_IP) != 0U) ? ESP_OK : ESP_ERR_TIMEOUT;
    case DEVICE_CONFIG_NETWORK_POLICY_WIFI_ONLY:
        bits = xEventGroupWaitBits(s_state.event_group,
                                   NETWORK_EVENT_WIFI_GOT_IP,
                                   pdFALSE,
                                   pdTRUE,
                                   pdMS_TO_TICKS(timeout_ms));
        return ((bits & NETWORK_EVENT_WIFI_GOT_IP) != 0U) ? ESP_OK : ESP_ERR_TIMEOUT;
    case DEVICE_CONFIG_NETWORK_POLICY_PREFER_ETHERNET: {
        uint32_t ethernet_wait_ms = network_prefer_ethernet_wait_ms(timeout_ms);
        uint32_t wifi_wait_ms = (timeout_ms > ethernet_wait_ms) ? (timeout_ms - ethernet_wait_ms) : 0U;

        bits = xEventGroupWaitBits(s_state.event_group,
                                   NETWORK_EVENT_ETH_GOT_IP,
                                   pdFALSE,
                                   pdTRUE,
                                   pdMS_TO_TICKS(ethernet_wait_ms));
        if ((bits & NETWORK_EVENT_ETH_GOT_IP) != 0U) {
            return ESP_OK;
        }

        if (!s_state.wifi_credentials.ssid_set) {
            ESP_LOGW(TAG, "Ethernet timed out and no WiFi fallback credentials are configured");
            return ESP_ERR_TIMEOUT;
        }

        ESP_LOGW(TAG,
                 "Ethernet did not acquire an IP within %u ms; switching to WiFi fallback",
                 (unsigned)ethernet_wait_ms);
        network_stop_ethernet();
        network_set_active_transport(NETWORK_TRANSPORT_WIFI);
        ESP_RETURN_ON_ERROR(network_start_wifi(), TAG, "Failed to start WiFi fallback");

        bits = xEventGroupWaitBits(s_state.event_group,
                                   NETWORK_EVENT_WIFI_GOT_IP,
                                   pdFALSE,
                                   pdTRUE,
                                   pdMS_TO_TICKS(wifi_wait_ms));
        return ((bits & NETWORK_EVENT_WIFI_GOT_IP) != 0U) ? ESP_OK : ESP_ERR_TIMEOUT;
    }
    default:
        return ESP_ERR_INVALID_STATE;
    }
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
    ESP_RETURN_ON_FALSE(status.connected, ESP_ERR_INVALID_STATE, TAG, "Active transport must have an IP before mDNS");

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
    ESP_LOGI(TAG, "Registered mDNS over %s", network_transport_name(status.transport));
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

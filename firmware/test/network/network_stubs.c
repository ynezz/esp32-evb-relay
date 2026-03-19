#include "network_test_stubs.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_event_stubs.h"
#include "esp_eth.h"
#include "esp_idf_stubs.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos_stubs.h"
#include "mdns.h"

struct esp_netif_obj {
    char hostname[DEVICE_CONFIG_HOSTNAME_MAX_LEN + 1];
};

struct esp_eth_handle_obj {
    int unused;
};

static device_config_network_policy_t s_policy;
static device_config_wifi_sta_credentials_t s_wifi_credentials;
static char s_hostname[DEVICE_CONFIG_HOSTNAME_MAX_LEN + 1];
static bool s_fail_eth_mac_new;
static esp_app_desc_t s_app_desc = {
    .project_name = "esp32-evb-relay",
    .version = "0.0.0-test",
};
static struct esp_netif_obj s_netifs[2];
static size_t s_netif_count;
static esp_eth_mac_t s_mac;
static esp_eth_phy_t s_phy;
static struct esp_eth_handle_obj s_eth_handle;
static int s_glue_token;

static esp_err_t network_test_eth_mac_del(esp_eth_mac_t *self)
{
    (void)self;
    return ESP_OK;
}

static esp_err_t network_test_eth_phy_del(esp_eth_phy_t *self)
{
    (void)self;
    return ESP_OK;
}

void network_test_stubs_reset(void)
{
    memset(&s_wifi_credentials, 0, sizeof(s_wifi_credentials));
    memset(s_hostname, 0, sizeof(s_hostname));
    memcpy(s_hostname, DEVICE_CONFIG_DEFAULT_HOSTNAME, sizeof(DEVICE_CONFIG_DEFAULT_HOSTNAME));
    s_policy = DEVICE_CONFIG_NETWORK_POLICY_ETHERNET_ONLY;
    s_fail_eth_mac_new = false;
    memset(s_netifs, 0, sizeof(s_netifs));
    s_netif_count = 0U;
    memset(&s_mac, 0, sizeof(s_mac));
    memset(&s_phy, 0, sizeof(s_phy));
    s_mac.del = network_test_eth_mac_del;
    s_phy.del = network_test_eth_phy_del;
    esp_event_stub_reset();
    freertos_stub_reset();
    esp_stub_reset_time_override();
    esp_stub_reset_restart_count();
}

void network_test_stubs_set_network_policy(device_config_network_policy_t policy)
{
    s_policy = policy;
}

void network_test_stubs_set_eth_mac_new_failure(bool fail)
{
    s_fail_eth_mac_new = fail;
}

esp_err_t device_config_get_hostname(char *buffer, size_t buffer_size)
{
    size_t hostname_len;

    if ((buffer == NULL) || (buffer_size == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    hostname_len = strlen(s_hostname) + 1U;
    if (buffer_size < hostname_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(buffer, s_hostname, hostname_len);
    return ESP_OK;
}

esp_err_t device_config_get_wifi_sta_credentials(device_config_wifi_sta_credentials_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out = s_wifi_credentials;
    return ESP_OK;
}

esp_err_t device_config_get_network_policy(device_config_network_policy_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out = s_policy;
    return ESP_OK;
}

const esp_app_desc_t *esp_app_get_description(void)
{
    return &s_app_desc;
}

esp_err_t esp_netif_init(void)
{
    return ESP_OK;
}

esp_netif_t *esp_netif_new(const esp_netif_config_t *config)
{
    esp_netif_t *netif;

    (void)config;

    if (s_netif_count >= (sizeof(s_netifs) / sizeof(s_netifs[0]))) {
        return NULL;
    }

    netif = &s_netifs[s_netif_count++];
    memset(netif, 0, sizeof(*netif));
    return netif;
}

void esp_netif_destroy(esp_netif_t *netif)
{
    (void)netif;
}

esp_err_t esp_netif_attach(esp_netif_t *netif, void *glue_handle)
{
    if ((netif == NULL) || (glue_handle == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

esp_err_t esp_netif_set_hostname(esp_netif_t *netif, const char *hostname)
{
    size_t hostname_len;

    if ((netif == NULL) || (hostname == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    hostname_len = strlen(hostname);
    if (hostname_len >= sizeof(netif->hostname)) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(netif->hostname, hostname, hostname_len + 1U);
    return ESP_OK;
}

esp_eth_mac_t *esp_eth_mac_new_esp32(const eth_esp32_emac_config_t *config,
                                     const eth_mac_config_t *mac_config)
{
    (void)config;
    (void)mac_config;

    return s_fail_eth_mac_new ? NULL : &s_mac;
}

esp_eth_phy_t *esp_eth_phy_new_lan87xx(const eth_phy_config_t *config)
{
    (void)config;
    return &s_phy;
}

esp_err_t esp_eth_driver_install(const esp_eth_config_t *config, esp_eth_handle_t *out_handle)
{
    (void)config;

    if (out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_handle = &s_eth_handle;
    return ESP_OK;
}

esp_eth_netif_glue_handle_t esp_eth_new_netif_glue(esp_eth_handle_t handle)
{
    return (handle != NULL) ? &s_glue_token : NULL;
}

esp_err_t esp_eth_del_netif_glue(esp_eth_netif_glue_handle_t glue)
{
    return (glue != NULL) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t esp_eth_driver_uninstall(esp_eth_handle_t handle)
{
    return (handle != NULL) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t esp_eth_start(esp_eth_handle_t handle)
{
    return (handle != NULL) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t esp_eth_stop(esp_eth_handle_t handle)
{
    return (handle != NULL) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t esp_eth_ioctl(esp_eth_handle_t handle, int cmd, void *data)
{
    (void)cmd;

    if ((handle == NULL) || (data == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(data, 0, 6U);
    return ESP_OK;
}

esp_err_t esp_netif_attach_wifi_station(esp_netif_t *netif)
{
    return (netif != NULL) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t esp_wifi_set_default_wifi_sta_handlers(void)
{
    return ESP_OK;
}

esp_err_t esp_wifi_clear_default_wifi_driver_and_handlers(esp_netif_t *netif)
{
    return (netif != NULL) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t esp_wifi_init(const wifi_init_config_t *config)
{
    (void)config;
    return ESP_OK;
}

esp_err_t esp_wifi_set_storage(wifi_storage_t storage)
{
    (void)storage;
    return ESP_OK;
}

esp_err_t esp_wifi_set_mode(wifi_mode_t mode)
{
    (void)mode;
    return ESP_OK;
}

esp_err_t esp_wifi_set_config(int interface, const wifi_config_t *config)
{
    (void)interface;
    return (config != NULL) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t esp_wifi_start(void)
{
    return ESP_OK;
}

esp_err_t esp_wifi_connect(void)
{
    return ESP_OK;
}

esp_err_t esp_wifi_stop(void)
{
    return ESP_OK;
}

esp_err_t esp_wifi_deinit(void)
{
    return ESP_OK;
}

esp_err_t mdns_init(void)
{
    return ESP_OK;
}

esp_err_t mdns_hostname_set(const char *hostname)
{
    return (hostname != NULL) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t mdns_instance_name_set(const char *instance_name)
{
    return (instance_name != NULL) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t mdns_service_add(const char *instance_name,
                           const char *service_type,
                           const char *proto,
                           uint16_t port,
                           const mdns_txt_item_t txt[],
                           size_t num_items)
{
    (void)txt;
    (void)num_items;

    if ((instance_name == NULL) || (service_type == NULL) || (proto == NULL) || (port == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

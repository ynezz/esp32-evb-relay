#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_CONFIG_DEFAULT_HOSTNAME "esp32-evb-relay"
#define DEVICE_CONFIG_HOSTNAME_MAX_LEN 63
#define DEVICE_CONFIG_API_TOKEN_MAX_LEN 255
#define DEVICE_CONFIG_WIFI_SSID_MAX_LEN 32
#define DEVICE_CONFIG_WIFI_PASSPHRASE_MAX_LEN 64
#define DEVICE_CONFIG_DEFAULT_POLL_INTERVAL_MS 100U
#define DEVICE_CONFIG_MIN_POLL_INTERVAL_MS 50U
#define DEVICE_CONFIG_MAX_POLL_INTERVAL_MS 10000U

typedef enum {
    DEVICE_CONFIG_KEY_API_TOKEN = 0,
    DEVICE_CONFIG_KEY_POLL_INTERVAL_MS,
    DEVICE_CONFIG_KEY_HOSTNAME,
    DEVICE_CONFIG_KEY_WIFI_SSID,
    DEVICE_CONFIG_KEY_WIFI_PASSPHRASE,
    DEVICE_CONFIG_KEY_NETWORK_POLICY,
    DEVICE_CONFIG_KEY_MODIO_BOOT_POLICY,
    DEVICE_CONFIG_KEY_COUNT,
} device_config_key_t;

typedef enum {
    DEVICE_CONFIG_MODIO_BOOT_POLICY_LEAVE_UNCHANGED = 0,
    DEVICE_CONFIG_MODIO_BOOT_POLICY_ALL_OFF = 1,
} device_config_modio_boot_policy_t;

typedef enum {
    DEVICE_CONFIG_NETWORK_POLICY_ETHERNET_ONLY = 0,
    DEVICE_CONFIG_NETWORK_POLICY_WIFI_ONLY,
    DEVICE_CONFIG_NETWORK_POLICY_PREFER_ETHERNET,
} device_config_network_policy_t;

typedef enum {
    DEVICE_CONFIG_APPLY_MODE_IMMEDIATE = 0,
    DEVICE_CONFIG_APPLY_MODE_RESTART_REQUIRED,
    DEVICE_CONFIG_APPLY_MODE_NEXT_BOOT,
} device_config_apply_mode_t;

typedef struct {
    device_config_apply_mode_t apply_mode;
} device_config_apply_result_t;

typedef struct {
    uint32_t poll_interval_ms;
    char hostname[DEVICE_CONFIG_HOSTNAME_MAX_LEN + 1];
    bool wifi_ssid_set;
    bool wifi_passphrase_set;
    device_config_network_policy_t network_policy;
    device_config_modio_boot_policy_t modio_boot_policy;
    bool api_token_set;
} device_config_snapshot_t;

typedef struct {
    bool ssid_set;
    bool passphrase_set;
    char ssid[DEVICE_CONFIG_WIFI_SSID_MAX_LEN + 1];
    char passphrase[DEVICE_CONFIG_WIFI_PASSPHRASE_MAX_LEN + 1];
} device_config_wifi_sta_credentials_t;

typedef struct {
    device_config_key_t key;
    const char *name;
    bool secret;
    device_config_apply_mode_t apply_mode;
} device_config_key_descriptor_t;

esp_err_t device_config_init(void);

esp_err_t device_config_get_snapshot(device_config_snapshot_t *out);
esp_err_t device_config_get_api_token(char *buffer, size_t buffer_size, bool *is_set);
esp_err_t device_config_set_api_token(const char *token, device_config_apply_result_t *result);

esp_err_t device_config_get_poll_interval_ms(uint32_t *out);
esp_err_t device_config_set_poll_interval_ms(uint32_t poll_interval_ms, device_config_apply_result_t *result);

esp_err_t device_config_get_hostname(char *buffer, size_t buffer_size);
esp_err_t device_config_set_hostname(const char *hostname, device_config_apply_result_t *result);

esp_err_t device_config_get_wifi_sta_credentials(device_config_wifi_sta_credentials_t *out);
esp_err_t device_config_set_wifi_sta_credentials(const char *ssid,
                                                 const char *passphrase,
                                                 device_config_apply_result_t *result);

esp_err_t device_config_get_network_policy(device_config_network_policy_t *out);
esp_err_t device_config_set_network_policy(device_config_network_policy_t policy,
                                           device_config_apply_result_t *result);

esp_err_t device_config_get_modio_boot_policy(device_config_modio_boot_policy_t *out);
esp_err_t device_config_set_modio_boot_policy(device_config_modio_boot_policy_t policy,
                                              device_config_apply_result_t *result);

const device_config_key_descriptor_t *device_config_get_key_descriptor(device_config_key_t key);
const char *device_config_apply_mode_to_string(device_config_apply_mode_t apply_mode);
const char *device_config_network_policy_to_string(device_config_network_policy_t policy);
esp_err_t device_config_parse_network_policy(const char *value,
                                             device_config_network_policy_t *out);
const char *device_config_modio_boot_policy_to_string(device_config_modio_boot_policy_t policy);
esp_err_t device_config_parse_modio_boot_policy(const char *value,
                                                device_config_modio_boot_policy_t *out);

#if defined(UNIT_TEST) || defined(DEVICE_CONFIG_ENABLE_TESTING_API)
void device_config_reset_for_testing(void);
#endif

#ifdef __cplusplus
}
#endif

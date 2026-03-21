#pragma once

#include <stdbool.h>

#include "device_config.h"
#include "esp_err.h"

typedef struct {
    bool credentials_present;
    bool credentials_clear;
    bool passphrase_present;
    char ssid[DEVICE_CONFIG_WIFI_SSID_MAX_LEN + 1U];
    char passphrase[DEVICE_CONFIG_WIFI_PASSPHRASE_MAX_LEN + 1U];
    bool network_policy_present;
    device_config_network_policy_t network_policy;
} rest_api_wifi_config_update_request_t;

esp_err_t rest_api_wifi_config_update_set_ssid(rest_api_wifi_config_update_request_t *request,
                                               const char *ssid,
                                               bool clear,
                                               const char **out_error_code,
                                               const char **out_error_message);

esp_err_t rest_api_wifi_config_update_set_passphrase(rest_api_wifi_config_update_request_t *request,
                                                     const char *passphrase,
                                                     const char **out_error_code,
                                                     const char **out_error_message);

esp_err_t rest_api_wifi_config_update_set_network_policy(rest_api_wifi_config_update_request_t *request,
                                                         const char *network_policy,
                                                         const char **out_error_code,
                                                         const char **out_error_message);

esp_err_t rest_api_wifi_config_update_validate(const rest_api_wifi_config_update_request_t *request,
                                               bool has_updates,
                                               const char **out_error_code,
                                               const char **out_error_message);

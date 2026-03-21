#include "rest_api_wifi_config_update.h"

#include <string.h>

static void rest_api_wifi_config_update_set_error(const char **out_code,
                                                  const char **out_message,
                                                  const char *code,
                                                  const char *message)
{
    if (out_code != NULL) {
        *out_code = code;
    }
    if (out_message != NULL) {
        *out_message = message;
    }
}

esp_err_t rest_api_wifi_config_update_set_ssid(rest_api_wifi_config_update_request_t *request,
                                               const char *ssid,
                                               bool clear,
                                               const char **out_error_code,
                                               const char **out_error_message)
{
    size_t ssid_len;

    if (request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    request->credentials_present = true;
    request->credentials_clear = clear;

    if (clear) {
        request->ssid[0] = '\0';
        request->passphrase[0] = '\0';
        return ESP_OK;
    }

    if (ssid == NULL) {
        rest_api_wifi_config_update_set_error(out_error_code,
                                              out_error_message,
                                              "INVALID_CONFIG_VALUE",
                                              "ssid must be a string or null");
        return ESP_ERR_INVALID_ARG;
    }

    ssid_len = strlen(ssid);
    if (ssid_len > DEVICE_CONFIG_WIFI_SSID_MAX_LEN) {
        rest_api_wifi_config_update_set_error(out_error_code,
                                              out_error_message,
                                              "INVALID_CONFIG_VALUE",
                                              "ssid exceeds the maximum length");
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(request->ssid, ssid, sizeof(request->ssid) - 1U);
    request->ssid[sizeof(request->ssid) - 1U] = '\0';
    return ESP_OK;
}

esp_err_t rest_api_wifi_config_update_set_passphrase(rest_api_wifi_config_update_request_t *request,
                                                     const char *passphrase,
                                                     const char **out_error_code,
                                                     const char **out_error_message)
{
    size_t passphrase_len;

    if (request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (passphrase == NULL) {
        rest_api_wifi_config_update_set_error(out_error_code,
                                              out_error_message,
                                              "INVALID_CONFIG_VALUE",
                                              "passphrase must be a string");
        return ESP_ERR_INVALID_ARG;
    }

    passphrase_len = strlen(passphrase);
    if (passphrase_len > DEVICE_CONFIG_WIFI_PASSPHRASE_MAX_LEN) {
        rest_api_wifi_config_update_set_error(out_error_code,
                                              out_error_message,
                                              "INVALID_CONFIG_VALUE",
                                              "passphrase exceeds the maximum length");
        return ESP_ERR_INVALID_ARG;
    }

    request->passphrase_present = true;
    strncpy(request->passphrase, passphrase, sizeof(request->passphrase) - 1U);
    request->passphrase[sizeof(request->passphrase) - 1U] = '\0';
    return ESP_OK;
}

esp_err_t rest_api_wifi_config_update_set_network_policy(rest_api_wifi_config_update_request_t *request,
                                                         const char *network_policy,
                                                         const char **out_error_code,
                                                         const char **out_error_message)
{
    if (request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (network_policy == NULL) {
        rest_api_wifi_config_update_set_error(out_error_code,
                                              out_error_message,
                                              "INVALID_CONFIG_VALUE",
                                              "network_policy must be a string");
        return ESP_ERR_INVALID_ARG;
    }

    if (device_config_parse_network_policy(network_policy, &request->network_policy) != ESP_OK) {
        rest_api_wifi_config_update_set_error(
            out_error_code,
            out_error_message,
            "INVALID_CONFIG_VALUE",
            "network_policy must be ethernet_only, wifi_only, or prefer_ethernet");
        return ESP_ERR_INVALID_ARG;
    }

    request->network_policy_present = true;
    return ESP_OK;
}

esp_err_t rest_api_wifi_config_update_validate(const rest_api_wifi_config_update_request_t *request,
                                               bool has_updates,
                                               const char **out_error_code,
                                               const char **out_error_message)
{
    if (request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (request->credentials_clear && request->passphrase_present) {
        rest_api_wifi_config_update_set_error(out_error_code,
                                              out_error_message,
                                              "INVALID_CONFIG_VALUE",
                                              "passphrase cannot be combined with ssid=null");
        return ESP_ERR_INVALID_ARG;
    }

    if (request->credentials_present && !request->credentials_clear && (request->ssid[0] == '\0')) {
        rest_api_wifi_config_update_set_error(out_error_code,
                                              out_error_message,
                                              "INVALID_CONFIG_VALUE",
                                              "ssid must not be empty");
        return ESP_ERR_INVALID_ARG;
    }

    if (request->passphrase_present && !request->credentials_present) {
        rest_api_wifi_config_update_set_error(out_error_code,
                                              out_error_message,
                                              "INVALID_CONFIG_VALUE",
                                              "passphrase requires ssid");
        return ESP_ERR_INVALID_ARG;
    }

    if (request->credentials_present && !request->credentials_clear && !request->passphrase_present) {
        rest_api_wifi_config_update_set_error(out_error_code,
                                              out_error_message,
                                              "INVALID_CONFIG_VALUE",
                                              "passphrase is required when setting WiFi credentials");
        return ESP_ERR_INVALID_ARG;
    }

    if (!has_updates) {
        rest_api_wifi_config_update_set_error(out_error_code,
                                              out_error_message,
                                              "INVALID_CONFIG",
                                              "Request body must update at least one supported key");
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

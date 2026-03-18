#include "auth.h"

#include <stdint.h>
#include <string.h>

#include "device_config.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"

#define AUTH_BEARER_PREFIX "Bearer "
#define AUTH_FIRST_BOOT_TOKEN_BYTES 16U
#define AUTH_FIRST_BOOT_TOKEN_LEN (AUTH_FIRST_BOOT_TOKEN_BYTES * 2U)

static const char *TAG = "auth";

static void auth_generate_token(char *buffer)
{
    static const char hex[] = "0123456789abcdef";
    uint8_t random_bytes[AUTH_FIRST_BOOT_TOKEN_BYTES];

    esp_fill_random(random_bytes, sizeof(random_bytes));
    for (size_t i = 0; i < sizeof(random_bytes); ++i) {
        buffer[i * 2U] = hex[random_bytes[i] >> 4];
        buffer[(i * 2U) + 1U] = hex[random_bytes[i] & 0x0FU];
    }
    buffer[sizeof(random_bytes) * 2U] = '\0';
}

static bool auth_constant_time_equals(const char *lhs, const char *rhs)
{
    size_t lhs_len = strnlen(lhs, DEVICE_CONFIG_API_TOKEN_MAX_LEN + 1U);
    size_t rhs_len = strnlen(rhs, DEVICE_CONFIG_API_TOKEN_MAX_LEN + 1U);
    size_t max_len = (lhs_len > rhs_len) ? lhs_len : rhs_len;
    unsigned diff = (unsigned)(lhs_len ^ rhs_len);

    for (size_t i = 0; i < max_len; ++i) {
        unsigned char lhs_ch = (i < lhs_len) ? (unsigned char)lhs[i] : 0U;
        unsigned char rhs_ch = (i < rhs_len) ? (unsigned char)rhs[i] : 0U;

        diff |= (unsigned)(lhs_ch ^ rhs_ch);
    }

    return diff == 0U;
}

static const char *auth_extract_bearer_token(const char *header_value)
{
    size_t prefix_len = sizeof(AUTH_BEARER_PREFIX) - 1U;

    if (strncmp(header_value, AUTH_BEARER_PREFIX, prefix_len) != 0) {
        return NULL;
    }

    header_value += prefix_len;
    return (header_value[0] != '\0') ? header_value : NULL;
}

esp_err_t auth_init(void)
{
    char token[DEVICE_CONFIG_API_TOKEN_MAX_LEN + 1U];
    bool is_set = false;
    esp_err_t err;

    err = device_config_get_api_token(token, sizeof(token), &is_set);
    if (err != ESP_OK) {
        return err;
    }

    if (is_set) {
        return ESP_OK;
    }

    auth_generate_token(token);
    err = device_config_set_api_token(token, NULL);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGW(TAG, "Generated first-boot API token: %s", token);
    return ESP_OK;
}

rest_api_auth_result_t auth_check(httpd_req_t *req, void *ctx)
{
    char configured_token[DEVICE_CONFIG_API_TOKEN_MAX_LEN + 1U];
    char header_value[sizeof(AUTH_BEARER_PREFIX) + DEVICE_CONFIG_API_TOKEN_MAX_LEN + 1U];
    const char *presented_token;
    bool is_set = false;
    size_t header_len;
    esp_err_t err;

    (void)ctx;
    if (req == NULL) {
        return REST_API_AUTH_RESULT_FORBIDDEN;
    }

    err = device_config_get_api_token(configured_token, sizeof(configured_token), &is_set);
    if ((err != ESP_OK) || !is_set) {
        ESP_LOGE(TAG, "No API token is configured");
        return REST_API_AUTH_RESULT_FORBIDDEN;
    }

    header_len = httpd_req_get_hdr_value_len(req, "Authorization");
    if ((header_len == 0U) || (header_len >= sizeof(header_value))) {
        return REST_API_AUTH_RESULT_UNAUTHORIZED;
    }

    err = httpd_req_get_hdr_value_str(req, "Authorization", header_value, sizeof(header_value));
    if (err != ESP_OK) {
        return REST_API_AUTH_RESULT_UNAUTHORIZED;
    }

    presented_token = auth_extract_bearer_token(header_value);
    if (presented_token == NULL) {
        return REST_API_AUTH_RESULT_UNAUTHORIZED;
    }

    return auth_constant_time_equals(configured_token, presented_token)
           ? REST_API_AUTH_RESULT_ALLOW
           : REST_API_AUTH_RESULT_FORBIDDEN;
}

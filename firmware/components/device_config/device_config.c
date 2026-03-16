#include "device_config.h"

#include <ctype.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#define DEVICE_CONFIG_NVS_NAMESPACE "device_cfg"

typedef struct {
    device_config_key_descriptor_t descriptor;
    const char *nvs_key;
} device_config_key_metadata_t;

typedef struct {
    char api_token[DEVICE_CONFIG_API_TOKEN_MAX_LEN + 1];
    uint32_t poll_interval_ms;
    char hostname[DEVICE_CONFIG_HOSTNAME_MAX_LEN + 1];
    device_config_modio_boot_policy_t modio_boot_policy;
} device_config_state_t;

static const char *TAG = "device_config";

static const device_config_key_metadata_t s_key_metadata[DEVICE_CONFIG_KEY_COUNT] = {
    [DEVICE_CONFIG_KEY_API_TOKEN] = {
        .descriptor =
            {
                .key = DEVICE_CONFIG_KEY_API_TOKEN,
                .name = "api_token",
                .secret = true,
                .live = true,
                .restart_required = false,
            },
        .nvs_key = "api_token",
    },
    [DEVICE_CONFIG_KEY_POLL_INTERVAL_MS] = {
        .descriptor =
            {
                .key = DEVICE_CONFIG_KEY_POLL_INTERVAL_MS,
                .name = "poll_interval_ms",
                .secret = false,
                .live = true,
                .restart_required = false,
            },
        .nvs_key = "poll_ms",
    },
    [DEVICE_CONFIG_KEY_HOSTNAME] = {
        .descriptor =
            {
                .key = DEVICE_CONFIG_KEY_HOSTNAME,
                .name = "hostname",
                .secret = false,
                .live = false,
                .restart_required = true,
            },
        .nvs_key = "hostname",
    },
    [DEVICE_CONFIG_KEY_MODIO_BOOT_POLICY] = {
        .descriptor =
            {
                .key = DEVICE_CONFIG_KEY_MODIO_BOOT_POLICY,
                .name = "modio_boot_policy",
                .secret = false,
                .live = false,
                .restart_required = false,
            },
        .nvs_key = "modio_policy",
    },
};

static StaticSemaphore_t s_lock_buffer;
static SemaphoreHandle_t s_lock;
static bool s_initialized;
static device_config_state_t s_state = {
    .api_token = "",
    .poll_interval_ms = DEVICE_CONFIG_DEFAULT_POLL_INTERVAL_MS,
    .hostname = DEVICE_CONFIG_DEFAULT_HOSTNAME,
    .modio_boot_policy = DEVICE_CONFIG_MODIO_BOOT_POLICY_LEAVE_UNCHANGED,
};

static void device_config_apply_defaults(device_config_state_t *state)
{
    state->api_token[0] = '\0';
    state->poll_interval_ms = DEVICE_CONFIG_DEFAULT_POLL_INTERVAL_MS;
    memcpy(state->hostname, DEVICE_CONFIG_DEFAULT_HOSTNAME, sizeof(DEVICE_CONFIG_DEFAULT_HOSTNAME));
    state->modio_boot_policy = DEVICE_CONFIG_MODIO_BOOT_POLICY_LEAVE_UNCHANGED;
}

static esp_err_t device_config_ensure_lock(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buffer);
    }

    return (s_lock != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t device_config_init_nvs_flash(void)
{
    esp_err_t err = nvs_flash_init();
    if ((err == ESP_ERR_NVS_NO_FREE_PAGES) || (err == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "Failed to erase NVS flash");
        err = nvs_flash_init();
    }

    return err;
}

static esp_err_t device_config_validate_poll_interval_ms(uint32_t poll_interval_ms)
{
    if ((poll_interval_ms < DEVICE_CONFIG_MIN_POLL_INTERVAL_MS) ||
        (poll_interval_ms > DEVICE_CONFIG_MAX_POLL_INTERVAL_MS)) {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static esp_err_t device_config_validate_hostname(const char *hostname)
{
    size_t length;

    if (hostname == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    length = strnlen(hostname, DEVICE_CONFIG_HOSTNAME_MAX_LEN + 2U);
    if ((length == 0U) || (length > DEVICE_CONFIG_HOSTNAME_MAX_LEN)) {
        return ESP_ERR_INVALID_ARG;
    }

    if ((hostname[0] == '-') || (hostname[length - 1U] == '-')) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < length; ++i) {
        unsigned char ch = (unsigned char)hostname[i];
        if (!isalnum(ch) && (ch != '-')) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    return ESP_OK;
}

static esp_err_t device_config_validate_api_token(const char *token)
{
    size_t length;

    if ((token == NULL) || (token[0] == '\0')) {
        return ESP_OK;
    }

    length = strnlen(token, DEVICE_CONFIG_API_TOKEN_MAX_LEN + 2U);
    if (length > DEVICE_CONFIG_API_TOKEN_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static void device_config_fill_apply_result(device_config_key_t key, device_config_apply_result_t *result)
{
    if (result == NULL) {
        return;
    }

    result->live = s_key_metadata[key].descriptor.live;
    result->restart_required = s_key_metadata[key].descriptor.restart_required;
}

static esp_err_t device_config_load_api_token(nvs_handle_t handle, device_config_state_t *state, bool *dirty)
{
    size_t required_size = sizeof(state->api_token);
    esp_err_t err = nvs_get_str(handle,
                                s_key_metadata[DEVICE_CONFIG_KEY_API_TOKEN].nvs_key,
                                state->api_token,
                                &required_size);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        state->api_token[0] = '\0';
        return ESP_OK;
    }

    if (err == ESP_ERR_NVS_INVALID_LENGTH) {
        ESP_LOGW(TAG, "Resetting invalid stored value for %s",
                 s_key_metadata[DEVICE_CONFIG_KEY_API_TOKEN].descriptor.name);
        state->api_token[0] = '\0';
        ESP_RETURN_ON_ERROR(nvs_erase_key(handle, s_key_metadata[DEVICE_CONFIG_KEY_API_TOKEN].nvs_key), TAG,
                            "Failed to erase invalid NVS key for %s",
                            s_key_metadata[DEVICE_CONFIG_KEY_API_TOKEN].descriptor.name);
        *dirty = true;
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(err, TAG, "Failed to load %s",
                        s_key_metadata[DEVICE_CONFIG_KEY_API_TOKEN].descriptor.name);

    if (device_config_validate_api_token(state->api_token) != ESP_OK) {
        ESP_LOGW(TAG, "Resetting invalid stored value for %s",
                 s_key_metadata[DEVICE_CONFIG_KEY_API_TOKEN].descriptor.name);
        state->api_token[0] = '\0';
        ESP_RETURN_ON_ERROR(nvs_erase_key(handle, s_key_metadata[DEVICE_CONFIG_KEY_API_TOKEN].nvs_key), TAG,
                            "Failed to erase invalid NVS key for %s",
                            s_key_metadata[DEVICE_CONFIG_KEY_API_TOKEN].descriptor.name);
        *dirty = true;
    }

    return ESP_OK;
}

static esp_err_t device_config_load_poll_interval_ms(nvs_handle_t handle, device_config_state_t *state, bool *dirty)
{
    uint32_t value = 0;
    esp_err_t err = nvs_get_u32(handle, s_key_metadata[DEVICE_CONFIG_KEY_POLL_INTERVAL_MS].nvs_key, &value);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        state->poll_interval_ms = DEVICE_CONFIG_DEFAULT_POLL_INTERVAL_MS;
        ESP_RETURN_ON_ERROR(nvs_set_u32(handle,
                                        s_key_metadata[DEVICE_CONFIG_KEY_POLL_INTERVAL_MS].nvs_key,
                                        state->poll_interval_ms),
                            TAG, "Failed to store default value for %s",
                            s_key_metadata[DEVICE_CONFIG_KEY_POLL_INTERVAL_MS].descriptor.name);
        *dirty = true;
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(err, TAG, "Failed to load %s",
                        s_key_metadata[DEVICE_CONFIG_KEY_POLL_INTERVAL_MS].descriptor.name);

    if (device_config_validate_poll_interval_ms(value) != ESP_OK) {
        ESP_LOGW(TAG, "Resetting invalid stored value for %s",
                 s_key_metadata[DEVICE_CONFIG_KEY_POLL_INTERVAL_MS].descriptor.name);
        value = DEVICE_CONFIG_DEFAULT_POLL_INTERVAL_MS;
        ESP_RETURN_ON_ERROR(nvs_set_u32(handle,
                                        s_key_metadata[DEVICE_CONFIG_KEY_POLL_INTERVAL_MS].nvs_key,
                                        value),
                            TAG, "Failed to store default value for %s",
                            s_key_metadata[DEVICE_CONFIG_KEY_POLL_INTERVAL_MS].descriptor.name);
        *dirty = true;
    }

    state->poll_interval_ms = value;
    return ESP_OK;
}

static esp_err_t device_config_load_hostname(nvs_handle_t handle, device_config_state_t *state, bool *dirty)
{
    size_t required_size = sizeof(state->hostname);
    esp_err_t err = nvs_get_str(handle,
                                s_key_metadata[DEVICE_CONFIG_KEY_HOSTNAME].nvs_key,
                                state->hostname,
                                &required_size);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        memcpy(state->hostname, DEVICE_CONFIG_DEFAULT_HOSTNAME, sizeof(DEVICE_CONFIG_DEFAULT_HOSTNAME));
        ESP_RETURN_ON_ERROR(nvs_set_str(handle,
                                        s_key_metadata[DEVICE_CONFIG_KEY_HOSTNAME].nvs_key,
                                        state->hostname),
                            TAG, "Failed to store default value for %s",
                            s_key_metadata[DEVICE_CONFIG_KEY_HOSTNAME].descriptor.name);
        *dirty = true;
        return ESP_OK;
    }

    if (err == ESP_ERR_NVS_INVALID_LENGTH) {
        ESP_LOGW(TAG, "Resetting invalid stored value for %s",
                 s_key_metadata[DEVICE_CONFIG_KEY_HOSTNAME].descriptor.name);
        memcpy(state->hostname, DEVICE_CONFIG_DEFAULT_HOSTNAME, sizeof(DEVICE_CONFIG_DEFAULT_HOSTNAME));
        ESP_RETURN_ON_ERROR(nvs_set_str(handle,
                                        s_key_metadata[DEVICE_CONFIG_KEY_HOSTNAME].nvs_key,
                                        state->hostname),
                            TAG, "Failed to store default value for %s",
                            s_key_metadata[DEVICE_CONFIG_KEY_HOSTNAME].descriptor.name);
        *dirty = true;
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(err, TAG, "Failed to load %s",
                        s_key_metadata[DEVICE_CONFIG_KEY_HOSTNAME].descriptor.name);

    if (device_config_validate_hostname(state->hostname) != ESP_OK) {
        ESP_LOGW(TAG, "Resetting invalid stored value for %s",
                 s_key_metadata[DEVICE_CONFIG_KEY_HOSTNAME].descriptor.name);
        memcpy(state->hostname, DEVICE_CONFIG_DEFAULT_HOSTNAME, sizeof(DEVICE_CONFIG_DEFAULT_HOSTNAME));
        ESP_RETURN_ON_ERROR(nvs_set_str(handle,
                                        s_key_metadata[DEVICE_CONFIG_KEY_HOSTNAME].nvs_key,
                                        state->hostname),
                            TAG, "Failed to store default value for %s",
                            s_key_metadata[DEVICE_CONFIG_KEY_HOSTNAME].descriptor.name);
        *dirty = true;
    }

    return ESP_OK;
}

static esp_err_t device_config_load_modio_boot_policy(nvs_handle_t handle,
                                                      device_config_state_t *state,
                                                      bool *dirty)
{
    uint8_t raw_policy = 0;
    esp_err_t err = nvs_get_u8(handle, s_key_metadata[DEVICE_CONFIG_KEY_MODIO_BOOT_POLICY].nvs_key, &raw_policy);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        state->modio_boot_policy = DEVICE_CONFIG_MODIO_BOOT_POLICY_LEAVE_UNCHANGED;
        ESP_RETURN_ON_ERROR(nvs_set_u8(handle,
                                       s_key_metadata[DEVICE_CONFIG_KEY_MODIO_BOOT_POLICY].nvs_key,
                                       (uint8_t)state->modio_boot_policy),
                            TAG, "Failed to store default value for %s",
                            s_key_metadata[DEVICE_CONFIG_KEY_MODIO_BOOT_POLICY].descriptor.name);
        *dirty = true;
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(err, TAG, "Failed to load %s",
                        s_key_metadata[DEVICE_CONFIG_KEY_MODIO_BOOT_POLICY].descriptor.name);

    if (raw_policy > (uint8_t)DEVICE_CONFIG_MODIO_BOOT_POLICY_ALL_OFF) {
        ESP_LOGW(TAG, "Resetting invalid stored value for %s",
                 s_key_metadata[DEVICE_CONFIG_KEY_MODIO_BOOT_POLICY].descriptor.name);
        state->modio_boot_policy = DEVICE_CONFIG_MODIO_BOOT_POLICY_LEAVE_UNCHANGED;
        ESP_RETURN_ON_ERROR(nvs_set_u8(handle,
                                       s_key_metadata[DEVICE_CONFIG_KEY_MODIO_BOOT_POLICY].nvs_key,
                                       (uint8_t)state->modio_boot_policy),
                            TAG, "Failed to store default value for %s",
                            s_key_metadata[DEVICE_CONFIG_KEY_MODIO_BOOT_POLICY].descriptor.name);
        *dirty = true;
        return ESP_OK;
    }

    state->modio_boot_policy = (device_config_modio_boot_policy_t)raw_policy;
    return ESP_OK;
}

static esp_err_t device_config_load_state(device_config_state_t *state)
{
    bool dirty = false;
    nvs_handle_t handle = 0;
    esp_err_t err;

    device_config_apply_defaults(state);

    err = nvs_open(DEVICE_CONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle);
    ESP_RETURN_ON_ERROR(err, TAG, "Failed to open NVS namespace");

    err = device_config_load_api_token(handle, state, &dirty);
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = device_config_load_poll_interval_ms(handle, state, &dirty);
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = device_config_load_hostname(handle, state, &dirty);
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = device_config_load_modio_boot_policy(handle, state, &dirty);
    if (err != ESP_OK) {
        goto cleanup;
    }

    if (dirty) {
        err = nvs_commit(handle);
    } else {
        err = ESP_OK;
    }

cleanup:
    nvs_close(handle);
    return err;
}

const device_config_key_descriptor_t *device_config_get_key_descriptor(device_config_key_t key)
{
    if ((key < 0) || (key >= DEVICE_CONFIG_KEY_COUNT)) {
        return NULL;
    }

    return &s_key_metadata[key].descriptor;
}

const char *device_config_modio_boot_policy_to_string(device_config_modio_boot_policy_t policy)
{
    switch (policy) {
    case DEVICE_CONFIG_MODIO_BOOT_POLICY_LEAVE_UNCHANGED:
        return "leave_unchanged";
    case DEVICE_CONFIG_MODIO_BOOT_POLICY_ALL_OFF:
        return "all_off";
    default:
        return "unknown";
    }
}

esp_err_t device_config_parse_modio_boot_policy(const char *value, device_config_modio_boot_policy_t *out)
{
    if ((value == NULL) || (out == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strcmp(value, "leave_unchanged") == 0) {
        *out = DEVICE_CONFIG_MODIO_BOOT_POLICY_LEAVE_UNCHANGED;
        return ESP_OK;
    }

    if (strcmp(value, "all_off") == 0) {
        *out = DEVICE_CONFIG_MODIO_BOOT_POLICY_ALL_OFF;
        return ESP_OK;
    }

    return ESP_ERR_INVALID_ARG;
}

esp_err_t device_config_init(void)
{
    esp_err_t err;
    device_config_state_t loaded_state;

    ESP_RETURN_ON_ERROR(device_config_ensure_lock(), TAG, "Failed to create config lock");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_initialized) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    err = device_config_init_nvs_flash();
    if (err != ESP_OK) {
        xSemaphoreGive(s_lock);
        return err;
    }

    err = device_config_load_state(&loaded_state);
    if (err == ESP_OK) {
        s_state = loaded_state;
        s_initialized = true;
    }

    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t device_config_get_snapshot(device_config_snapshot_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "Snapshot output buffer is required");
    ESP_RETURN_ON_ERROR(device_config_init(), TAG, "Failed to initialize device config");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->poll_interval_ms = s_state.poll_interval_ms;
    memcpy(out->hostname, s_state.hostname, sizeof(out->hostname));
    out->modio_boot_policy = s_state.modio_boot_policy;
    out->api_token_set = (s_state.api_token[0] != '\0');
    xSemaphoreGive(s_lock);

    return ESP_OK;
}

esp_err_t device_config_get_api_token(char *buffer, size_t buffer_size, bool *is_set)
{
    size_t token_len;

    ESP_RETURN_ON_FALSE(buffer != NULL, ESP_ERR_INVALID_ARG, TAG, "API token buffer is required");
    ESP_RETURN_ON_FALSE(buffer_size > 0U, ESP_ERR_INVALID_SIZE, TAG, "API token buffer is too small");
    ESP_RETURN_ON_ERROR(device_config_init(), TAG, "Failed to initialize device config");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    token_len = strnlen(s_state.api_token, sizeof(s_state.api_token));
    if (is_set != NULL) {
        *is_set = (token_len > 0U);
    }

    if (token_len >= buffer_size) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(buffer, s_state.api_token, token_len + 1U);
    xSemaphoreGive(s_lock);

    return ESP_OK;
}

esp_err_t device_config_set_api_token(const char *token, device_config_apply_result_t *result)
{
    bool clear_value = (token == NULL) || (token[0] == '\0');
    nvs_handle_t handle = 0;
    esp_err_t err;

    ESP_RETURN_ON_ERROR(device_config_validate_api_token(token), TAG, "Invalid API token");
    ESP_RETURN_ON_ERROR(device_config_init(), TAG, "Failed to initialize device config");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (clear_value && (s_state.api_token[0] == '\0')) {
        device_config_fill_apply_result(DEVICE_CONFIG_KEY_API_TOKEN, result);
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    if (!clear_value && (strcmp(s_state.api_token, token) == 0)) {
        device_config_fill_apply_result(DEVICE_CONFIG_KEY_API_TOKEN, result);
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    err = nvs_open(DEVICE_CONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        xSemaphoreGive(s_lock);
        return err;
    }

    if (clear_value) {
        err = nvs_erase_key(handle, s_key_metadata[DEVICE_CONFIG_KEY_API_TOKEN].nvs_key);
        if ((err == ESP_OK) || (err == ESP_ERR_NVS_NOT_FOUND)) {
            err = ESP_OK;
        }
    } else {
        err = nvs_set_str(handle, s_key_metadata[DEVICE_CONFIG_KEY_API_TOKEN].nvs_key, token);
    }

    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    if (err == ESP_OK) {
        if (clear_value) {
            s_state.api_token[0] = '\0';
        } else {
            size_t token_len = strnlen(token, DEVICE_CONFIG_API_TOKEN_MAX_LEN);
            memcpy(s_state.api_token, token, token_len);
            s_state.api_token[token_len] = '\0';
        }
        device_config_fill_apply_result(DEVICE_CONFIG_KEY_API_TOKEN, result);
    }

    nvs_close(handle);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t device_config_get_poll_interval_ms(uint32_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "Poll interval output buffer is required");
    ESP_RETURN_ON_ERROR(device_config_init(), TAG, "Failed to initialize device config");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_state.poll_interval_ms;
    xSemaphoreGive(s_lock);

    return ESP_OK;
}

esp_err_t device_config_set_poll_interval_ms(uint32_t poll_interval_ms, device_config_apply_result_t *result)
{
    nvs_handle_t handle = 0;
    esp_err_t err;

    ESP_RETURN_ON_ERROR(device_config_validate_poll_interval_ms(poll_interval_ms), TAG,
                        "Invalid poll interval");
    ESP_RETURN_ON_ERROR(device_config_init(), TAG, "Failed to initialize device config");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state.poll_interval_ms == poll_interval_ms) {
        device_config_fill_apply_result(DEVICE_CONFIG_KEY_POLL_INTERVAL_MS, result);
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    err = nvs_open(DEVICE_CONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        xSemaphoreGive(s_lock);
        return err;
    }

    err = nvs_set_u32(handle, s_key_metadata[DEVICE_CONFIG_KEY_POLL_INTERVAL_MS].nvs_key, poll_interval_ms);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    if (err == ESP_OK) {
        s_state.poll_interval_ms = poll_interval_ms;
        device_config_fill_apply_result(DEVICE_CONFIG_KEY_POLL_INTERVAL_MS, result);
    }

    nvs_close(handle);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t device_config_get_hostname(char *buffer, size_t buffer_size)
{
    size_t hostname_len;

    ESP_RETURN_ON_FALSE(buffer != NULL, ESP_ERR_INVALID_ARG, TAG, "Hostname buffer is required");
    ESP_RETURN_ON_FALSE(buffer_size > 0U, ESP_ERR_INVALID_SIZE, TAG, "Hostname buffer is too small");
    ESP_RETURN_ON_ERROR(device_config_init(), TAG, "Failed to initialize device config");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    hostname_len = strnlen(s_state.hostname, sizeof(s_state.hostname));
    if (hostname_len >= buffer_size) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(buffer, s_state.hostname, hostname_len + 1U);
    xSemaphoreGive(s_lock);

    return ESP_OK;
}

esp_err_t device_config_set_hostname(const char *hostname, device_config_apply_result_t *result)
{
    nvs_handle_t handle = 0;
    esp_err_t err;

    ESP_RETURN_ON_ERROR(device_config_validate_hostname(hostname), TAG, "Invalid hostname");
    ESP_RETURN_ON_ERROR(device_config_init(), TAG, "Failed to initialize device config");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (strcmp(s_state.hostname, hostname) == 0) {
        device_config_fill_apply_result(DEVICE_CONFIG_KEY_HOSTNAME, result);
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    err = nvs_open(DEVICE_CONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        xSemaphoreGive(s_lock);
        return err;
    }

    err = nvs_set_str(handle, s_key_metadata[DEVICE_CONFIG_KEY_HOSTNAME].nvs_key, hostname);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    if (err == ESP_OK) {
        size_t hostname_len = strnlen(hostname, DEVICE_CONFIG_HOSTNAME_MAX_LEN);
        memcpy(s_state.hostname, hostname, hostname_len);
        s_state.hostname[hostname_len] = '\0';
        device_config_fill_apply_result(DEVICE_CONFIG_KEY_HOSTNAME, result);
    }

    nvs_close(handle);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t device_config_get_modio_boot_policy(device_config_modio_boot_policy_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "Boot policy output buffer is required");
    ESP_RETURN_ON_ERROR(device_config_init(), TAG, "Failed to initialize device config");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_state.modio_boot_policy;
    xSemaphoreGive(s_lock);

    return ESP_OK;
}

esp_err_t device_config_set_modio_boot_policy(device_config_modio_boot_policy_t policy,
                                              device_config_apply_result_t *result)
{
    nvs_handle_t handle = 0;
    esp_err_t err;

    ESP_RETURN_ON_FALSE(policy <= DEVICE_CONFIG_MODIO_BOOT_POLICY_ALL_OFF, ESP_ERR_INVALID_ARG, TAG,
                        "Invalid MOD-IO boot policy");
    ESP_RETURN_ON_ERROR(device_config_init(), TAG, "Failed to initialize device config");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state.modio_boot_policy == policy) {
        device_config_fill_apply_result(DEVICE_CONFIG_KEY_MODIO_BOOT_POLICY, result);
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    err = nvs_open(DEVICE_CONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        xSemaphoreGive(s_lock);
        return err;
    }

    err = nvs_set_u8(handle, s_key_metadata[DEVICE_CONFIG_KEY_MODIO_BOOT_POLICY].nvs_key, (uint8_t)policy);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    if (err == ESP_OK) {
        s_state.modio_boot_policy = policy;
        device_config_fill_apply_result(DEVICE_CONFIG_KEY_MODIO_BOOT_POLICY, result);
    }

    nvs_close(handle);
    xSemaphoreGive(s_lock);
    return err;
}

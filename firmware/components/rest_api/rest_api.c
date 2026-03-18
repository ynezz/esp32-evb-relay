#include "rest_api.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "device_config.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mod_io.h"
#include "relay.h"
#include "rest_api_events.h"

ESP_EVENT_DEFINE_BASE(EVB_RELAY_EVENT);

static const char *TAG = "rest_api";

static httpd_handle_t s_server;
static rest_api_config_t s_config;

static rest_api_auth_result_t rest_api_authorize_request(httpd_req_t *req);

#define REST_API_MAX_REQUEST_BODY_LEN 512U

static const char *REST_API_RELAYS_URI = "/api/v1/relays";
static const char *REST_API_ONBOARD_RELAYS_URI = "/api/v1/relays/onboard";
static const char *REST_API_ONBOARD_RELAY_ID_PREFIX = "/api/v1/relays/onboard/";
static const char *REST_API_ONBOARD_RELAY_TOGGLE_SUFFIX = "/toggle";
static const char *REST_API_MODIO_RELAYS_URI = "/api/v1/relays/modio";
static const char *REST_API_MODIO_RELAY_ID_PREFIX = "/api/v1/relays/modio/";
static const char *REST_API_CONFIG_URI = "/api/v1/config";

typedef struct {
    bool api_token_present;
    bool api_token_clear;
    char api_token[DEVICE_CONFIG_API_TOKEN_MAX_LEN + 1U];
    bool poll_interval_present;
    uint32_t poll_interval_ms;
    bool hostname_present;
    char hostname[DEVICE_CONFIG_HOSTNAME_MAX_LEN + 1U];
    bool modio_boot_policy_present;
    device_config_modio_boot_policy_t modio_boot_policy;
} rest_api_config_update_request_t;

static const char *rest_api_http_status_text(int http_status)
{
    switch (http_status) {
    case 200:
        return "200 OK";
    case 400:
        return "400 Bad Request";
    case 401:
        return "401 Unauthorized";
    case 403:
        return "403 Forbidden";
    case 404:
        return "404 Not Found";
    case 409:
        return "409 Conflict";
    case 500:
        return "500 Internal Server Error";
    case 503:
        return "503 Service Unavailable";
    default:
        return "500 Internal Server Error";
    }
}

const char *rest_api_modio_sync_to_string(rest_api_modio_sync_t sync_state)
{
    switch (sync_state) {
    case REST_API_MODIO_SYNC_ABSENT:
        return "absent";
    case REST_API_MODIO_SYNC_SYNCHRONIZED:
        return "synchronized";
    default:
        return "absent";
    }
}

static rest_api_modio_sync_t rest_api_modio_sync_from_driver(mod_io_relay_sync_t relay_sync)
{
    switch (relay_sync) {
    case MOD_IO_RELAY_SYNC_ABSENT:
        return REST_API_MODIO_SYNC_ABSENT;
    case MOD_IO_RELAY_SYNC_SYNCHRONIZED:
        return REST_API_MODIO_SYNC_SYNCHRONIZED;
    default:
        return REST_API_MODIO_SYNC_ABSENT;
    }
}

static esp_err_t rest_api_build_status_view(rest_api_status_view_t *status)
{
    ESP_RETURN_ON_FALSE(status != NULL, ESP_ERR_INVALID_ARG, TAG, "Status output buffer is required");
    ESP_RETURN_ON_FALSE(s_config.status_provider != NULL,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "Status provider is required");

    memset(status, 0, sizeof(*status));
    return s_config.status_provider(status, s_config.status_ctx);
}

static esp_err_t rest_api_require_authenticated_status(httpd_req_t *req,
                                                       rest_api_status_view_t *out_status)
{
    rest_api_auth_result_t auth_result;
    esp_err_t err;

    ESP_RETURN_ON_FALSE(req != NULL, ESP_ERR_INVALID_ARG, TAG, "HTTP request is required");
    ESP_RETURN_ON_FALSE(out_status != NULL, ESP_ERR_INVALID_ARG, TAG, "Status output buffer is required");

    auth_result = rest_api_authorize_request(req);
    if (auth_result == REST_API_AUTH_RESULT_UNAUTHORIZED) {
        return rest_api_send_error(req, 401, "AUTH_REQUIRED", "Authentication required", false);
    }

    if (auth_result == REST_API_AUTH_RESULT_FORBIDDEN) {
        return rest_api_send_error(req, 403, "AUTH_FORBIDDEN", "Access denied", false);
    }

    err = rest_api_build_status_view(out_status);
    if (err != ESP_OK) {
        return rest_api_send_error(req, 500, "STATUS_UNAVAILABLE", "Failed to gather status", true);
    }

    return ESP_OK;
}

static void rest_api_try_attach_device_context_headers(httpd_req_t *req,
                                                       const rest_api_status_view_t *status,
                                                       bool authenticated)
{
    const esp_app_desc_t *app_desc;

    if (!authenticated) {
        return;
    }

    app_desc = esp_app_get_description();
    if (app_desc != NULL) {
        httpd_resp_set_hdr(req, "X-FW-Version", app_desc->version);
    }

    httpd_resp_set_hdr(req, "X-ModIO-Present", status->modio_present ? "true" : "false");
    httpd_resp_set_hdr(req, "X-ModIO-Sync", rest_api_modio_sync_to_string(status->modio_sync));
}

static esp_err_t rest_api_send_json_response(httpd_req_t *req,
                                             int http_status,
                                             cJSON *root,
                                             const rest_api_status_view_t *status,
                                             bool authenticated)
{
    char *response = NULL;
    esp_err_t err;

    ESP_RETURN_ON_FALSE(req != NULL, ESP_ERR_INVALID_ARG, TAG, "HTTP request is required");
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_INVALID_ARG, TAG, "JSON root is required");

    response = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (response == NULL) {
        return rest_api_send_error(req,
                                   500,
                                   "INTERNAL_ERROR",
                                   "Failed to serialize JSON response",
                                   authenticated);
    }

    httpd_resp_set_status(req, rest_api_http_status_text(http_status));
    httpd_resp_set_type(req, "application/json");
    if (authenticated && (status != NULL)) {
        rest_api_try_attach_device_context_headers(req, status, true);
    }

    err = httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    cJSON_free(response);
    return err;
}

static rest_api_auth_result_t rest_api_authorize_request(httpd_req_t *req)
{
    (void)req;

    if (s_config.auth_handler == NULL) {
        return REST_API_AUTH_RESULT_FORBIDDEN;
    }

    return s_config.auth_handler(req, s_config.auth_ctx);
}

static bool rest_api_parse_id_from_uri_with_suffix(const char *uri,
                                                   const char *prefix,
                                                   const char *suffix,
                                                   uint32_t *out_id)
{
    char id_buffer[16];
    const char *id_start;
    size_t prefix_len;
    size_t suffix_len;
    size_t uri_len;
    size_t id_len;

    if ((uri == NULL) || (prefix == NULL) || (suffix == NULL) || (out_id == NULL)) {
        return false;
    }

    prefix_len = strlen(prefix);
    suffix_len = strlen(suffix);
    uri_len = strlen(uri);

    if ((uri_len <= (prefix_len + suffix_len)) || (strncmp(uri, prefix, prefix_len) != 0)) {
        return false;
    }

    if (strcmp(uri + uri_len - suffix_len, suffix) != 0) {
        return false;
    }

    id_start = uri + prefix_len;
    id_len = uri_len - prefix_len - suffix_len;
    if ((id_len == 0U) || (id_len >= sizeof(id_buffer))) {
        return false;
    }

    memcpy(id_buffer, id_start, id_len);
    id_buffer[id_len] = '\0';
    return rest_api_parse_id_from_uri(id_buffer, "", out_id);
}

static esp_err_t rest_api_parse_onboard_relay_id(httpd_req_t *req,
                                                 const char *prefix,
                                                 const char *suffix,
                                                 uint8_t *out_relay_id)
{
    uint32_t parsed_id = 0;
    bool parsed;
    esp_err_t err;

    ESP_RETURN_ON_FALSE(req != NULL, ESP_ERR_INVALID_ARG, TAG, "HTTP request is required");
    ESP_RETURN_ON_FALSE(prefix != NULL, ESP_ERR_INVALID_ARG, TAG, "Relay URI prefix is required");
    ESP_RETURN_ON_FALSE(out_relay_id != NULL, ESP_ERR_INVALID_ARG, TAG, "Relay id output is required");

    parsed = (suffix == NULL) ? rest_api_parse_id_from_uri(req->uri, prefix, &parsed_id)
             : rest_api_parse_id_from_uri_with_suffix(req->uri, prefix, suffix, &parsed_id);
    if (!parsed || (parsed_id == 0U) || (parsed_id > RELAY_COUNT)) {
        err = rest_api_send_error(req, 404, "RELAY_NOT_FOUND", "Relay not found", true);
        if (err != ESP_OK) {
            return err;
        }
        return ESP_ERR_NOT_FOUND;
    }

    *out_relay_id = (uint8_t)parsed_id;
    return ESP_OK;
}

static esp_err_t rest_api_parse_modio_relay_id(httpd_req_t *req, uint8_t *out_relay_id)
{
    uint32_t parsed_id = 0;
    esp_err_t err;

    ESP_RETURN_ON_FALSE(req != NULL, ESP_ERR_INVALID_ARG, TAG, "HTTP request is required");
    ESP_RETURN_ON_FALSE(out_relay_id != NULL, ESP_ERR_INVALID_ARG, TAG, "Relay id output is required");

    if (!rest_api_parse_id_from_uri(req->uri, REST_API_MODIO_RELAY_ID_PREFIX, &parsed_id) ||
            (parsed_id == 0U) || (parsed_id > MOD_IO_RELAY_COUNT)) {
        err = rest_api_send_error(req, 404, "RELAY_NOT_FOUND", "Relay not found", true);
        if (err != ESP_OK) {
            return err;
        }
        return ESP_ERR_NOT_FOUND;
    }

    *out_relay_id = (uint8_t)parsed_id;
    return ESP_OK;
}

static cJSON *rest_api_create_onboard_relay_object(uint8_t relay_id, bool state)
{
    cJSON *relay = cJSON_CreateObject();

    if (relay == NULL) {
        return NULL;
    }

    cJSON_AddStringToObject(relay, "group", "onboard");
    cJSON_AddNumberToObject(relay, "id", relay_id);
    cJSON_AddBoolToObject(relay, "state", state);
    return relay;
}

static cJSON *rest_api_create_relay_with_sync_object(const char *group,
                                                     uint8_t relay_id,
                                                     bool state,
                                                     const char *sync)
{
    cJSON *relay = cJSON_CreateObject();

    if ((relay == NULL) || (group == NULL)) {
        cJSON_Delete(relay);
        return NULL;
    }

    cJSON_AddStringToObject(relay, "group", group);
    cJSON_AddNumberToObject(relay, "id", relay_id);
    cJSON_AddBoolToObject(relay, "state", state);
    if (sync != NULL) {
        cJSON_AddStringToObject(relay, "sync", sync);
    } else {
        cJSON_AddNullToObject(relay, "sync");
    }
    return relay;
}

static esp_err_t rest_api_send_onboard_relay_response(httpd_req_t *req,
                                                      const rest_api_status_view_t *status,
                                                      uint8_t relay_id,
                                                      bool relay_state)
{
    cJSON *root = NULL;
    cJSON *relay = NULL;

    root = cJSON_CreateObject();
    relay = rest_api_create_onboard_relay_object(relay_id, relay_state);
    if ((root == NULL) || (relay == NULL)) {
        cJSON_Delete(root);
        cJSON_Delete(relay);
        return rest_api_send_error(req,
                                   500,
                                   "INTERNAL_ERROR",
                                   "Failed to allocate JSON response",
                                   true);
    }

    cJSON_AddItemToObject(root, "relay", relay);
    return rest_api_send_json_response(req, 200, root, status, true);
}

static esp_err_t rest_api_send_modio_relay_response(httpd_req_t *req,
                                                    const rest_api_status_view_t *status,
                                                    uint8_t relay_id,
                                                    bool relay_state)
{
    cJSON *root = NULL;
    cJSON *relay = NULL;

    root = cJSON_CreateObject();
    relay = rest_api_create_relay_with_sync_object("modio",
                                                   relay_id,
                                                   relay_state,
                                                   rest_api_modio_sync_to_string(status->modio_sync));
    if ((root == NULL) || (relay == NULL)) {
        cJSON_Delete(root);
        cJSON_Delete(relay);
        return rest_api_send_error(req,
                                   500,
                                   "INTERNAL_ERROR",
                                   "Failed to allocate JSON response",
                                   true);
    }

    cJSON_AddItemToObject(root, "relay", relay);
    return rest_api_send_json_response(req, 200, root, status, true);
}

static esp_err_t rest_api_append_modio_relay_objects(cJSON *relays,
                                                     uint8_t relay_mask,
                                                     const char *sync)
{
    ESP_RETURN_ON_FALSE(relays != NULL, ESP_ERR_INVALID_ARG, TAG, "Relay array is required");

    for (uint8_t relay_id = 1U; relay_id <= MOD_IO_RELAY_COUNT; ++relay_id) {
        cJSON *relay = NULL;
        bool state = (relay_mask & (uint8_t)(1U << (relay_id - 1U))) != 0U;

        relay = rest_api_create_relay_with_sync_object("modio", relay_id, state, sync);
        if (relay == NULL) {
            return ESP_ERR_NO_MEM;
        }

        cJSON_AddItemToArray(relays, relay);
    }

    return ESP_OK;
}

static esp_err_t rest_api_append_combined_relay_objects(cJSON *relays,
                                                        uint8_t onboard_mask,
                                                        bool include_modio,
                                                        uint8_t modio_mask,
                                                        const char *modio_sync)
{
    ESP_RETURN_ON_FALSE(relays != NULL, ESP_ERR_INVALID_ARG, TAG, "Relay array is required");

    for (uint8_t relay_id = 1U; relay_id <= RELAY_COUNT; ++relay_id) {
        cJSON *relay = NULL;
        bool state = (onboard_mask & (uint8_t)(1U << (relay_id - 1U))) != 0U;

        relay = rest_api_create_relay_with_sync_object("onboard", relay_id, state, NULL);
        if (relay == NULL) {
            return ESP_ERR_NO_MEM;
        }

        cJSON_AddItemToArray(relays, relay);
    }

    if (include_modio) {
        return rest_api_append_modio_relay_objects(relays, modio_mask, modio_sync);
    }

    return ESP_OK;
}

static esp_err_t rest_api_read_request_body(httpd_req_t *req,
                                            char *buffer,
                                            size_t buffer_size,
                                            size_t *out_len)
{
    size_t remaining;
    size_t offset = 0;

    ESP_RETURN_ON_FALSE(req != NULL, ESP_ERR_INVALID_ARG, TAG, "HTTP request is required");
    ESP_RETURN_ON_FALSE(buffer != NULL, ESP_ERR_INVALID_ARG, TAG, "Request body buffer is required");
    ESP_RETURN_ON_FALSE(buffer_size > 1U, ESP_ERR_INVALID_ARG, TAG, "Request body buffer is too small");

    if ((req->content_len <= 0) || ((size_t)req->content_len >= buffer_size)) {
        return ESP_ERR_INVALID_SIZE;
    }

    remaining = (size_t)req->content_len;
    while (remaining > 0U) {
        int received;
        size_t chunk_len = remaining;

        if (chunk_len > (buffer_size - offset - 1U)) {
            chunk_len = buffer_size - offset - 1U;
        }

        received = httpd_req_recv(req, buffer + offset, chunk_len);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (received <= 0) {
            return ESP_FAIL;
        }

        remaining -= (size_t)received;
        offset += (size_t)received;
    }

    buffer[offset] = '\0';
    if (out_len != NULL) {
        *out_len = offset;
    }

    return ESP_OK;
}

static esp_err_t rest_api_parse_boolean_state_request(httpd_req_t *req, bool *out_state)
{
    char request_body[REST_API_MAX_REQUEST_BODY_LEN];
    size_t request_len = 0;
    cJSON *root = NULL;
    cJSON *state = NULL;
    esp_err_t err;

    ESP_RETURN_ON_FALSE(req != NULL, ESP_ERR_INVALID_ARG, TAG, "HTTP request is required");
    ESP_RETURN_ON_FALSE(out_state != NULL, ESP_ERR_INVALID_ARG, TAG, "State output is required");

    err = rest_api_read_request_body(req, request_body, sizeof(request_body), &request_len);
    if (err == ESP_ERR_INVALID_SIZE) {
        return rest_api_send_error(req,
                                   400,
                                   "INVALID_BODY",
                                   "Request body is missing or too large",
                                   true);
    }
    if (err != ESP_OK) {
        return rest_api_send_error(req,
                                   400,
                                   "INVALID_BODY",
                                   "Failed to read request body",
                                   true);
    }

    root = cJSON_ParseWithLength(request_body, request_len);
    if (root == NULL) {
        return rest_api_send_error(req, 400, "INVALID_JSON", "Request body must be valid JSON", true);
    }

    state = cJSON_GetObjectItemCaseSensitive(root, "state");
    if (!cJSON_IsBool(state)) {
        cJSON_Delete(root);
        return rest_api_send_error(req,
                                   400,
                                   "INVALID_RELAY_STATE",
                                   "Request body must contain boolean state",
                                   true);
    }

    *out_state = cJSON_IsTrue(state);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t rest_api_parse_modio_relay_states_request(httpd_req_t *req, uint8_t *out_relay_mask)
{
    char request_body[REST_API_MAX_REQUEST_BODY_LEN];
    size_t request_len = 0;
    cJSON *root = NULL;
    cJSON *states = NULL;
    esp_err_t err;
    uint8_t relay_mask = 0;

    ESP_RETURN_ON_FALSE(req != NULL, ESP_ERR_INVALID_ARG, TAG, "HTTP request is required");
    ESP_RETURN_ON_FALSE(out_relay_mask != NULL, ESP_ERR_INVALID_ARG, TAG, "Relay mask output is required");

    err = rest_api_read_request_body(req, request_body, sizeof(request_body), &request_len);
    if (err == ESP_ERR_INVALID_SIZE) {
        return rest_api_send_error(req,
                                   400,
                                   "INVALID_BODY",
                                   "Request body is missing or too large",
                                   true);
    }
    if (err != ESP_OK) {
        return rest_api_send_error(req,
                                   400,
                                   "INVALID_BODY",
                                   "Failed to read request body",
                                   true);
    }

    root = cJSON_ParseWithLength(request_body, request_len);
    if (root == NULL) {
        return rest_api_send_error(req, 400, "INVALID_JSON", "Request body must be valid JSON", true);
    }

    states = cJSON_GetObjectItemCaseSensitive(root, "states");
    if (!cJSON_IsArray(states) || (cJSON_GetArraySize(states) != MOD_IO_RELAY_COUNT)) {
        cJSON_Delete(root);
        return rest_api_send_error(req,
                                   400,
                                   "INVALID_RELAY_STATE",
                                   "Request body must contain exactly 4 relay states",
                                   true);
    }

    for (uint8_t relay_id = 1U; relay_id <= MOD_IO_RELAY_COUNT; ++relay_id) {
        cJSON *state = cJSON_GetArrayItem(states, relay_id - 1U);

        if (!cJSON_IsBool(state)) {
            cJSON_Delete(root);
            return rest_api_send_error(req,
                                       400,
                                       "INVALID_RELAY_STATE",
                                       "Relay states must be booleans",
                                       true);
        }

        if (cJSON_IsTrue(state)) {
            relay_mask |= (uint8_t)(1U << (relay_id - 1U));
        }
    }

    *out_relay_mask = relay_mask;
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t rest_api_get_onboard_relay_mask(uint8_t *out_mask)
{
    uint8_t relay_mask = 0;

    ESP_RETURN_ON_FALSE(out_mask != NULL, ESP_ERR_INVALID_ARG, TAG, "Relay mask output is required");

    for (uint8_t relay_id = 1U; relay_id <= RELAY_COUNT; ++relay_id) {
        bool relay_state = false;
        esp_err_t err = relay_get(relay_id, &relay_state);

        if (err != ESP_OK) {
            return err;
        }

        if (relay_state) {
            relay_mask |= (uint8_t)(1U << (relay_id - 1U));
        }
    }

    *out_mask = relay_mask;
    return ESP_OK;
}

static esp_err_t rest_api_sync_status_with_modio_driver(rest_api_status_view_t *status,
                                                        uint8_t *out_relay_mask)
{
    mod_io_status_t modio_status;
    esp_err_t err;

    ESP_RETURN_ON_FALSE(status != NULL, ESP_ERR_INVALID_ARG, TAG, "Status view is required");

    if (!status->modio_present) {
        status->modio_sync = REST_API_MODIO_SYNC_ABSENT;
        if (out_relay_mask != NULL) {
            *out_relay_mask = 0U;
        }
        return ESP_ERR_NOT_FOUND;
    }

    err = mod_io_get_status(&modio_status);
    if (err != ESP_OK) {
        return err;
    }

    status->modio_present = modio_status.present;
    status->modio_sync = rest_api_modio_sync_from_driver(modio_status.relay_sync);
    if (!modio_status.present) {
        if (out_relay_mask != NULL) {
            *out_relay_mask = 0U;
        }
        return ESP_ERR_NOT_FOUND;
    }

    if (out_relay_mask != NULL) {
        *out_relay_mask = modio_status.relay_mask;
    }

    return ESP_OK;
}

static const char *rest_api_config_response_key_name(device_config_key_t key)
{
    if (key == DEVICE_CONFIG_KEY_API_TOKEN) {
        return "api_token_set";
    }

    return device_config_get_key_descriptor(key)->name;
}

static bool rest_api_config_apply_mode_is_live(device_config_apply_mode_t apply_mode)
{
    return apply_mode == DEVICE_CONFIG_APPLY_MODE_IMMEDIATE;
}

static cJSON *rest_api_create_config_snapshot_object(const device_config_snapshot_t *snapshot)
{
    cJSON *config = NULL;

    ESP_RETURN_ON_FALSE(snapshot != NULL, NULL, TAG, "Config snapshot is required");

    config = cJSON_CreateObject();
    if (config == NULL) {
        return NULL;
    }

    cJSON_AddNumberToObject(config, "poll_interval_ms", (double)snapshot->poll_interval_ms);
    cJSON_AddStringToObject(config, "hostname", snapshot->hostname);
    cJSON_AddStringToObject(config,
                            "modio_boot_policy",
                            device_config_modio_boot_policy_to_string(snapshot->modio_boot_policy));
    cJSON_AddBoolToObject(config, "api_token_set", snapshot->api_token_set);
    return config;
}

static cJSON *rest_api_create_config_change_value(device_config_key_t key,
                                                  const device_config_snapshot_t *snapshot)
{
    ESP_RETURN_ON_FALSE(snapshot != NULL, NULL, TAG, "Config snapshot is required");

    switch (key) {
    case DEVICE_CONFIG_KEY_API_TOKEN:
        return cJSON_CreateBool(snapshot->api_token_set);
    case DEVICE_CONFIG_KEY_POLL_INTERVAL_MS:
        return cJSON_CreateNumber((double)snapshot->poll_interval_ms);
    case DEVICE_CONFIG_KEY_HOSTNAME:
        return cJSON_CreateString(snapshot->hostname);
    case DEVICE_CONFIG_KEY_MODIO_BOOT_POLICY:
        return cJSON_CreateString(device_config_modio_boot_policy_to_string(snapshot->modio_boot_policy));
    default:
        return NULL;
    }
}

static esp_err_t rest_api_append_config_change(cJSON *changes,
                                               device_config_key_t key,
                                               const device_config_snapshot_t *before,
                                               const device_config_snapshot_t *after,
                                               const device_config_apply_result_t *result)
{
    cJSON *change = NULL;
    cJSON *old_value = NULL;
    cJSON *new_value = NULL;

    ESP_RETURN_ON_FALSE(changes != NULL, ESP_ERR_INVALID_ARG, TAG, "Changes array is required");
    ESP_RETURN_ON_FALSE(before != NULL, ESP_ERR_INVALID_ARG, TAG, "Before snapshot is required");
    ESP_RETURN_ON_FALSE(after != NULL, ESP_ERR_INVALID_ARG, TAG, "After snapshot is required");
    ESP_RETURN_ON_FALSE(result != NULL, ESP_ERR_INVALID_ARG, TAG, "Apply result is required");

    change = cJSON_CreateObject();
    old_value = rest_api_create_config_change_value(key, before);
    new_value = rest_api_create_config_change_value(key, after);
    if ((change == NULL) || (old_value == NULL) || (new_value == NULL)) {
        cJSON_Delete(change);
        cJSON_Delete(old_value);
        cJSON_Delete(new_value);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(change, "key", rest_api_config_response_key_name(key));
    cJSON_AddItemToObject(change, "old", old_value);
    cJSON_AddItemToObject(change, "new", new_value);
    cJSON_AddBoolToObject(change, "live", rest_api_config_apply_mode_is_live(result->apply_mode));
    cJSON_AddItemToArray(changes, change);
    return ESP_OK;
}

static void rest_api_set_config_parse_error(const char **out_code,
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

static esp_err_t rest_api_parse_config_update_request(const cJSON *root,
                                                      rest_api_config_update_request_t *out_request,
                                                      const char **out_error_code,
                                                      const char **out_error_message)
{
    bool has_updates = false;
    const cJSON *item = NULL;

    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_INVALID_ARG, TAG, "Config JSON root is required");
    ESP_RETURN_ON_FALSE(out_request != NULL, ESP_ERR_INVALID_ARG, TAG, "Config request output is required");

    if (!cJSON_IsObject(root)) {
        rest_api_set_config_parse_error(out_error_code,
                                        out_error_message,
                                        "INVALID_CONFIG",
                                        "Request body must be a JSON object");
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_request, 0, sizeof(*out_request));
    cJSON_ArrayForEach(item, root) {
        if ((item == NULL) || (item->string == NULL)) {
            continue;
        }

        if (strcmp(item->string, "api_token") == 0) {
            has_updates = true;
            out_request->api_token_present = true;
            if (cJSON_IsNull(item)) {
                out_request->api_token_clear = true;
                out_request->api_token[0] = '\0';
                continue;
            }
            if (!cJSON_IsString(item) || (item->valuestring == NULL)) {
                rest_api_set_config_parse_error(out_error_code,
                                                out_error_message,
                                                "INVALID_CONFIG_VALUE",
                                                "api_token must be a string or null");
                return ESP_ERR_INVALID_ARG;
            }
            if (strlen(item->valuestring) > DEVICE_CONFIG_API_TOKEN_MAX_LEN) {
                rest_api_set_config_parse_error(out_error_code,
                                                out_error_message,
                                                "INVALID_CONFIG_VALUE",
                                                "api_token exceeds the maximum length");
                return ESP_ERR_INVALID_ARG;
            }
            strncpy(out_request->api_token, item->valuestring, sizeof(out_request->api_token) - 1U);
            continue;
        }

        if (strcmp(item->string, "poll_interval_ms") == 0) {
            double value = 0;

            has_updates = true;
            if (!cJSON_IsNumber(item)) {
                rest_api_set_config_parse_error(out_error_code,
                                                out_error_message,
                                                "INVALID_CONFIG_VALUE",
                                                "poll_interval_ms must be an integer");
                return ESP_ERR_INVALID_ARG;
            }

            value = item->valuedouble;
            if ((value < 0.0) || (value > (double)UINT32_MAX) || ((double)(uint32_t)value != value)) {
                rest_api_set_config_parse_error(out_error_code,
                                                out_error_message,
                                                "INVALID_CONFIG_VALUE",
                                                "poll_interval_ms must be an integer");
                return ESP_ERR_INVALID_ARG;
            }

            out_request->poll_interval_present = true;
            out_request->poll_interval_ms = (uint32_t)value;
            continue;
        }

        if (strcmp(item->string, "hostname") == 0) {
            has_updates = true;
            if (!cJSON_IsString(item) || (item->valuestring == NULL)) {
                rest_api_set_config_parse_error(out_error_code,
                                                out_error_message,
                                                "INVALID_CONFIG_VALUE",
                                                "hostname must be a string");
                return ESP_ERR_INVALID_ARG;
            }
            if (strlen(item->valuestring) > DEVICE_CONFIG_HOSTNAME_MAX_LEN) {
                rest_api_set_config_parse_error(out_error_code,
                                                out_error_message,
                                                "INVALID_CONFIG_VALUE",
                                                "hostname exceeds the maximum length");
                return ESP_ERR_INVALID_ARG;
            }
            out_request->hostname_present = true;
            strncpy(out_request->hostname, item->valuestring, sizeof(out_request->hostname) - 1U);
            continue;
        }

        if (strcmp(item->string, "modio_boot_policy") == 0) {
            has_updates = true;
            if (!cJSON_IsString(item) || (item->valuestring == NULL)) {
                rest_api_set_config_parse_error(out_error_code,
                                                out_error_message,
                                                "INVALID_CONFIG_VALUE",
                                                "modio_boot_policy must be a string");
                return ESP_ERR_INVALID_ARG;
            }
            if (device_config_parse_modio_boot_policy(item->valuestring, &out_request->modio_boot_policy)
                    != ESP_OK) {
                rest_api_set_config_parse_error(out_error_code,
                                                out_error_message,
                                                "INVALID_CONFIG_VALUE",
                                                "modio_boot_policy must be leave_unchanged or all_off");
                return ESP_ERR_INVALID_ARG;
            }
            out_request->modio_boot_policy_present = true;
            continue;
        }

        rest_api_set_config_parse_error(out_error_code,
                                        out_error_message,
                                        "INVALID_CONFIG_KEY",
                                        "Request body contains an unsupported config key");
        return ESP_ERR_INVALID_ARG;
    }

    if (!has_updates) {
        rest_api_set_config_parse_error(out_error_code,
                                        out_error_message,
                                        "INVALID_CONFIG",
                                        "Request body must update at least one supported key");
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static esp_err_t rest_api_config_handler(httpd_req_t *req)
{
    rest_api_status_view_t status;
    device_config_snapshot_t snapshot;
    cJSON *root = NULL;
    cJSON *config = NULL;
    esp_err_t err;

    err = rest_api_require_authenticated_status(req, &status);
    if (err != ESP_OK) {
        return err;
    }

    err = device_config_get_snapshot(&snapshot);
    if (err != ESP_OK) {
        return rest_api_send_error(req, 500, "CONFIG_UNAVAILABLE", "Failed to read device config", true);
    }

    root = cJSON_CreateObject();
    config = rest_api_create_config_snapshot_object(&snapshot);
    if ((root == NULL) || (config == NULL)) {
        cJSON_Delete(root);
        cJSON_Delete(config);
        return rest_api_send_error(req, 500, "INTERNAL_ERROR", "Failed to allocate JSON response", true);
    }

    cJSON_AddItemToObject(root, "config", config);
    return rest_api_send_json_response(req, 200, root, &status, true);
}

static esp_err_t rest_api_config_update_handler(httpd_req_t *req)
{
    rest_api_status_view_t status;
    device_config_snapshot_t before_snapshot;
    device_config_snapshot_t after_snapshot;
    rest_api_config_update_request_t update_request;
    device_config_apply_result_t apply_results[DEVICE_CONFIG_KEY_COUNT] = {0};
    bool requested[DEVICE_CONFIG_KEY_COUNT] = {0};
    const char *error_code = NULL;
    const char *error_message = NULL;
    char request_body[REST_API_MAX_REQUEST_BODY_LEN];
    size_t request_len = 0;
    cJSON *request_root = NULL;
    cJSON *response_root = NULL;
    cJSON *changes = NULL;
    bool restart_required = false;
    esp_err_t err;

    err = rest_api_require_authenticated_status(req, &status);
    if (err != ESP_OK) {
        return err;
    }

    err = rest_api_read_request_body(req, request_body, sizeof(request_body), &request_len);
    if (err == ESP_ERR_INVALID_SIZE) {
        return rest_api_send_error(req,
                                   400,
                                   "INVALID_BODY",
                                   "Request body is missing or too large",
                                   true);
    }
    if (err != ESP_OK) {
        return rest_api_send_error(req,
                                   400,
                                   "INVALID_BODY",
                                   "Failed to read request body",
                                   true);
    }

    request_root = cJSON_ParseWithLength(request_body, request_len);
    if (request_root == NULL) {
        return rest_api_send_error(req, 400, "INVALID_JSON", "Request body must be valid JSON", true);
    }

    err = rest_api_parse_config_update_request(request_root,
                                               &update_request,
                                               &error_code,
                                               &error_message);
    if (err != ESP_OK) {
        cJSON_Delete(request_root);
        return rest_api_send_error(req,
                                   400,
                                   (error_code != NULL) ? error_code : "INVALID_CONFIG",
                                   (error_message != NULL) ? error_message : "Invalid config request",
                                   true);
    }

    err = device_config_get_snapshot(&before_snapshot);
    if (err != ESP_OK) {
        cJSON_Delete(request_root);
        return rest_api_send_error(req, 500, "CONFIG_UNAVAILABLE", "Failed to read device config", true);
    }

    if (update_request.api_token_present) {
        requested[DEVICE_CONFIG_KEY_API_TOKEN] = true;
        err = device_config_set_api_token(update_request.api_token_clear ? NULL : update_request.api_token,
                                          &apply_results[DEVICE_CONFIG_KEY_API_TOKEN]);
        if (err == ESP_ERR_INVALID_ARG) {
            cJSON_Delete(request_root);
            return rest_api_send_error(req, 400, "INVALID_CONFIG_VALUE", "api_token is invalid", true);
        }
        if (err != ESP_OK) {
            cJSON_Delete(request_root);
            return rest_api_send_error(req, 500, "CONFIG_UPDATE_FAILED", "Failed to update api_token", true);
        }
    }

    if (update_request.poll_interval_present) {
        requested[DEVICE_CONFIG_KEY_POLL_INTERVAL_MS] = true;
        err = device_config_set_poll_interval_ms(update_request.poll_interval_ms,
                                                 &apply_results[DEVICE_CONFIG_KEY_POLL_INTERVAL_MS]);
        if (err == ESP_ERR_INVALID_ARG) {
            cJSON_Delete(request_root);
            return rest_api_send_error(req,
                                       400,
                                       "INVALID_CONFIG_VALUE",
                                       "poll_interval_ms is invalid",
                                       true);
        }
        if (err != ESP_OK) {
            cJSON_Delete(request_root);
            return rest_api_send_error(req,
                                       500,
                                       "CONFIG_UPDATE_FAILED",
                                       "Failed to update poll_interval_ms",
                                       true);
        }
    }

    if (update_request.hostname_present) {
        requested[DEVICE_CONFIG_KEY_HOSTNAME] = true;
        err = device_config_set_hostname(update_request.hostname, &apply_results[DEVICE_CONFIG_KEY_HOSTNAME]);
        if (err == ESP_ERR_INVALID_ARG) {
            cJSON_Delete(request_root);
            return rest_api_send_error(req, 400, "INVALID_CONFIG_VALUE", "hostname is invalid", true);
        }
        if (err != ESP_OK) {
            cJSON_Delete(request_root);
            return rest_api_send_error(req, 500, "CONFIG_UPDATE_FAILED", "Failed to update hostname", true);
        }
    }

    if (update_request.modio_boot_policy_present) {
        requested[DEVICE_CONFIG_KEY_MODIO_BOOT_POLICY] = true;
        err = device_config_set_modio_boot_policy(update_request.modio_boot_policy,
                                                  &apply_results[DEVICE_CONFIG_KEY_MODIO_BOOT_POLICY]);
        if (err == ESP_ERR_INVALID_ARG) {
            cJSON_Delete(request_root);
            return rest_api_send_error(req,
                                       400,
                                       "INVALID_CONFIG_VALUE",
                                       "modio_boot_policy is invalid",
                                       true);
        }
        if (err != ESP_OK) {
            cJSON_Delete(request_root);
            return rest_api_send_error(req,
                                       500,
                                       "CONFIG_UPDATE_FAILED",
                                       "Failed to update modio_boot_policy",
                                       true);
        }
    }

    err = device_config_get_snapshot(&after_snapshot);
    cJSON_Delete(request_root);
    if (err != ESP_OK) {
        return rest_api_send_error(req, 500, "CONFIG_UNAVAILABLE", "Failed to read device config", true);
    }

    response_root = cJSON_CreateObject();
    changes = cJSON_CreateArray();
    if ((response_root == NULL) || (changes == NULL)) {
        cJSON_Delete(response_root);
        cJSON_Delete(changes);
        return rest_api_send_error(req, 500, "INTERNAL_ERROR", "Failed to allocate JSON response", true);
    }

    for (device_config_key_t key = 0; key < DEVICE_CONFIG_KEY_COUNT; ++key) {
        if (!requested[key]) {
            continue;
        }

        err = rest_api_append_config_change(changes,
                                            key,
                                            &before_snapshot,
                                            &after_snapshot,
                                            &apply_results[key]);
        if (err != ESP_OK) {
            cJSON_Delete(response_root);
            cJSON_Delete(changes);
            return rest_api_send_error(req, 500, "INTERNAL_ERROR", "Failed to allocate JSON response", true);
        }

        if (!rest_api_config_apply_mode_is_live(apply_results[key].apply_mode)) {
            restart_required = true;
        }
    }

    cJSON_AddItemToObject(response_root, "changes", changes);
    cJSON_AddBoolToObject(response_root, "restart_required", restart_required);
    return rest_api_send_json_response(req, 200, response_root, &status, true);
}

static esp_err_t rest_api_onboard_relays_handler(httpd_req_t *req)
{
    rest_api_status_view_t status;
    cJSON *root = NULL;
    cJSON *relays = NULL;
    esp_err_t err;

    err = rest_api_require_authenticated_status(req, &status);
    if (err != ESP_OK) {
        return err;
    }

    root = cJSON_CreateObject();
    relays = cJSON_CreateArray();
    if ((root == NULL) || (relays == NULL)) {
        cJSON_Delete(root);
        cJSON_Delete(relays);
        return rest_api_send_error(req,
                                   500,
                                   "INTERNAL_ERROR",
                                   "Failed to allocate JSON response",
                                   true);
    }

    for (uint8_t relay_id = 1U; relay_id <= RELAY_COUNT; ++relay_id) {
        cJSON *relay = NULL;
        bool relay_state = false;

        err = relay_get(relay_id, &relay_state);
        if (err != ESP_OK) {
            cJSON_Delete(root);
            cJSON_Delete(relays);
            return rest_api_send_error(req,
                                       500,
                                       "RELAY_UNAVAILABLE",
                                       "Failed to read relay state",
                                       true);
        }

        relay = rest_api_create_onboard_relay_object(relay_id, relay_state);
        if (relay == NULL) {
            cJSON_Delete(root);
            cJSON_Delete(relays);
            return rest_api_send_error(req,
                                       500,
                                       "INTERNAL_ERROR",
                                       "Failed to allocate JSON response",
                                       true);
        }

        cJSON_AddItemToArray(relays, relay);
    }

    cJSON_AddItemToObject(root, "relays", relays);
    return rest_api_send_json_response(req, 200, root, &status, true);
}

static esp_err_t rest_api_onboard_relay_set_handler(httpd_req_t *req)
{
    rest_api_status_view_t status;
    uint8_t relay_id = 0;
    bool requested_state = false;
    bool actual_state = false;
    esp_err_t err;

    err = rest_api_require_authenticated_status(req, &status);
    if (err != ESP_OK) {
        return err;
    }

    err = rest_api_parse_onboard_relay_id(req, REST_API_ONBOARD_RELAY_ID_PREFIX, NULL, &relay_id);
    if (err != ESP_OK) {
        return err;
    }

    err = rest_api_parse_boolean_state_request(req, &requested_state);
    if (err != ESP_OK) {
        return err;
    }

    err = relay_set(relay_id, requested_state);
    if (err != ESP_OK) {
        return rest_api_send_error(req, 500, "RELAY_SET_FAILED", "Failed to set relay state", true);
    }

    err = relay_get(relay_id, &actual_state);
    if (err != ESP_OK) {
        return rest_api_send_error(req, 500, "RELAY_UNAVAILABLE", "Failed to read relay state", true);
    }

    return rest_api_send_onboard_relay_response(req, &status, relay_id, actual_state);
}

static esp_err_t rest_api_onboard_relay_toggle_handler(httpd_req_t *req)
{
    rest_api_status_view_t status;
    uint8_t relay_id = 0;
    bool actual_state = false;
    esp_err_t err;

    err = rest_api_require_authenticated_status(req, &status);
    if (err != ESP_OK) {
        return err;
    }

    err = rest_api_parse_onboard_relay_id(req,
                                          REST_API_ONBOARD_RELAY_ID_PREFIX,
                                          REST_API_ONBOARD_RELAY_TOGGLE_SUFFIX,
                                          &relay_id);
    if (err != ESP_OK) {
        return err;
    }

    err = relay_toggle(relay_id);
    if (err != ESP_OK) {
        return rest_api_send_error(req, 500, "RELAY_TOGGLE_FAILED", "Failed to toggle relay", true);
    }

    err = relay_get(relay_id, &actual_state);
    if (err != ESP_OK) {
        return rest_api_send_error(req, 500, "RELAY_UNAVAILABLE", "Failed to read relay state", true);
    }

    return rest_api_send_onboard_relay_response(req, &status, relay_id, actual_state);
}

static esp_err_t rest_api_relays_handler(httpd_req_t *req)
{
    rest_api_status_view_t status;
    cJSON *root = NULL;
    cJSON *relays = NULL;
    uint8_t onboard_mask = 0;
    uint8_t modio_mask = 0;
    esp_err_t err;

    err = rest_api_require_authenticated_status(req, &status);
    if (err != ESP_OK) {
        return err;
    }

    err = rest_api_get_onboard_relay_mask(&onboard_mask);
    if (err != ESP_OK) {
        return rest_api_send_error(req,
                                   500,
                                   "RELAY_UNAVAILABLE",
                                   "Failed to read relay state",
                                   true);
    }

    if (status.modio_present) {
        err = rest_api_sync_status_with_modio_driver(&status, &modio_mask);
        if ((err != ESP_OK) && (err != ESP_ERR_NOT_FOUND)) {
            return rest_api_send_error(req,
                                       500,
                                       "MODIO_UNAVAILABLE",
                                       "Failed to read MOD-IO relay state",
                                       true);
        }
    } else {
        status.modio_sync = REST_API_MODIO_SYNC_ABSENT;
    }

    root = cJSON_CreateObject();
    relays = cJSON_CreateArray();
    if ((root == NULL) || (relays == NULL)) {
        cJSON_Delete(root);
        cJSON_Delete(relays);
        return rest_api_send_error(req,
                                   500,
                                   "INTERNAL_ERROR",
                                   "Failed to allocate JSON response",
                                   true);
    }

    cJSON_AddBoolToObject(root, "modio_present", status.modio_present);
    cJSON_AddStringToObject(root, "modio_sync", rest_api_modio_sync_to_string(status.modio_sync));

    err = rest_api_append_combined_relay_objects(relays,
                                                 onboard_mask,
                                                 status.modio_present,
                                                 modio_mask,
                                                 rest_api_modio_sync_to_string(status.modio_sync));
    if (err != ESP_OK) {
        cJSON_Delete(root);
        cJSON_Delete(relays);
        return rest_api_send_error(req,
                                   500,
                                   "INTERNAL_ERROR",
                                   "Failed to allocate JSON response",
                                   true);
    }

    cJSON_AddItemToObject(root, "relays", relays);
    return rest_api_send_json_response(req, 200, root, &status, true);
}

static esp_err_t rest_api_modio_relays_handler(httpd_req_t *req)
{
    rest_api_status_view_t status;
    cJSON *root = NULL;
    cJSON *relays = NULL;
    uint8_t relay_mask = 0;
    esp_err_t err;

    err = rest_api_require_authenticated_status(req, &status);
    if (err != ESP_OK) {
        return err;
    }

    err = rest_api_sync_status_with_modio_driver(&status, &relay_mask);
    if (err == ESP_ERR_NOT_FOUND) {
        return rest_api_send_error(req, 503, "MODIO_NOT_PRESENT", "MOD-IO is not present", true);
    }
    if (err != ESP_OK) {
        return rest_api_send_error(req,
                                   500,
                                   "MODIO_UNAVAILABLE",
                                   "Failed to read MOD-IO relay state",
                                   true);
    }

    root = cJSON_CreateObject();
    relays = cJSON_CreateArray();
    if ((root == NULL) || (relays == NULL)) {
        cJSON_Delete(root);
        cJSON_Delete(relays);
        return rest_api_send_error(req,
                                   500,
                                   "INTERNAL_ERROR",
                                   "Failed to allocate JSON response",
                                   true);
    }

    err = rest_api_append_modio_relay_objects(relays,
                                              relay_mask,
                                              rest_api_modio_sync_to_string(status.modio_sync));
    if (err != ESP_OK) {
        cJSON_Delete(root);
        cJSON_Delete(relays);
        return rest_api_send_error(req,
                                   500,
                                   "INTERNAL_ERROR",
                                   "Failed to allocate JSON response",
                                   true);
    }

    cJSON_AddItemToObject(root, "relays", relays);
    return rest_api_send_json_response(req, 200, root, &status, true);
}

static esp_err_t rest_api_modio_relay_set_handler(httpd_req_t *req)
{
    rest_api_status_view_t status;
    uint8_t relay_id = 0;
    uint8_t relay_mask = 0;
    bool requested_state = false;
    bool actual_state = false;
    esp_err_t err;

    err = rest_api_require_authenticated_status(req, &status);
    if (err != ESP_OK) {
        return err;
    }

    err = rest_api_parse_modio_relay_id(req, &relay_id);
    if (err != ESP_OK) {
        return err;
    }

    err = rest_api_parse_boolean_state_request(req, &requested_state);
    if (err != ESP_OK) {
        return err;
    }

    err = rest_api_sync_status_with_modio_driver(&status, &relay_mask);
    if (err == ESP_ERR_NOT_FOUND) {
        return rest_api_send_error(req, 503, "MODIO_NOT_PRESENT", "MOD-IO is not present", true);
    }
    if (err != ESP_OK) {
        return rest_api_send_error(req,
                                   500,
                                   "MODIO_UNAVAILABLE",
                                   "Failed to read MOD-IO relay state",
                                   true);
    }

    if (requested_state) {
        relay_mask |= (uint8_t)(1U << (relay_id - 1U));
    } else {
        relay_mask &= (uint8_t)~(1U << (relay_id - 1U));
    }

    err = mod_io_set_relays(relay_mask);
    if (err == ESP_ERR_NOT_FOUND) {
        return rest_api_send_error(req, 503, "MODIO_NOT_PRESENT", "MOD-IO is not present", true);
    }
    if (err != ESP_OK) {
        return rest_api_send_error(req,
                                   500,
                                   "MODIO_SET_FAILED",
                                   "Failed to set MOD-IO relay state",
                                   true);
    }

    err = rest_api_sync_status_with_modio_driver(&status, &relay_mask);
    if (err == ESP_ERR_NOT_FOUND) {
        return rest_api_send_error(req, 503, "MODIO_NOT_PRESENT", "MOD-IO is not present", true);
    }
    if (err != ESP_OK) {
        return rest_api_send_error(req,
                                   500,
                                   "MODIO_UNAVAILABLE",
                                   "Failed to read MOD-IO relay state",
                                   true);
    }

    actual_state = (relay_mask & (uint8_t)(1U << (relay_id - 1U))) != 0U;
    return rest_api_send_modio_relay_response(req, &status, relay_id, actual_state);
}

static esp_err_t rest_api_modio_relays_set_handler(httpd_req_t *req)
{
    rest_api_status_view_t status;
    cJSON *root = NULL;
    cJSON *relays = NULL;
    uint8_t relay_mask = 0;
    esp_err_t err;

    err = rest_api_require_authenticated_status(req, &status);
    if (err != ESP_OK) {
        return err;
    }

    err = rest_api_parse_modio_relay_states_request(req, &relay_mask);
    if (err != ESP_OK) {
        return err;
    }

    err = rest_api_sync_status_with_modio_driver(&status, NULL);
    if (err == ESP_ERR_NOT_FOUND) {
        return rest_api_send_error(req, 503, "MODIO_NOT_PRESENT", "MOD-IO is not present", true);
    }
    if (err != ESP_OK) {
        return rest_api_send_error(req,
                                   500,
                                   "MODIO_UNAVAILABLE",
                                   "Failed to read MOD-IO relay state",
                                   true);
    }

    err = mod_io_set_relays(relay_mask);
    if (err == ESP_ERR_NOT_FOUND) {
        return rest_api_send_error(req, 503, "MODIO_NOT_PRESENT", "MOD-IO is not present", true);
    }
    if (err != ESP_OK) {
        return rest_api_send_error(req,
                                   500,
                                   "MODIO_SET_FAILED",
                                   "Failed to set MOD-IO relay state",
                                   true);
    }

    err = rest_api_sync_status_with_modio_driver(&status, &relay_mask);
    if (err == ESP_ERR_NOT_FOUND) {
        return rest_api_send_error(req, 503, "MODIO_NOT_PRESENT", "MOD-IO is not present", true);
    }
    if (err != ESP_OK) {
        return rest_api_send_error(req,
                                   500,
                                   "MODIO_UNAVAILABLE",
                                   "Failed to read MOD-IO relay state",
                                   true);
    }

    root = cJSON_CreateObject();
    relays = cJSON_CreateArray();
    if ((root == NULL) || (relays == NULL)) {
        cJSON_Delete(root);
        cJSON_Delete(relays);
        return rest_api_send_error(req,
                                   500,
                                   "INTERNAL_ERROR",
                                   "Failed to allocate JSON response",
                                   true);
    }

    err = rest_api_append_modio_relay_objects(relays,
                                              relay_mask,
                                              rest_api_modio_sync_to_string(status.modio_sync));
    if (err != ESP_OK) {
        cJSON_Delete(root);
        cJSON_Delete(relays);
        return rest_api_send_error(req,
                                   500,
                                   "INTERNAL_ERROR",
                                   "Failed to allocate JSON response",
                                   true);
    }

    cJSON_AddItemToObject(root, "relays", relays);
    return rest_api_send_json_response(req, 200, root, &status, true);
}

static esp_err_t rest_api_status_handler(httpd_req_t *req)
{
    rest_api_status_view_t status;
    cJSON *root = NULL;
    cJSON *network = NULL;
    cJSON *modio = NULL;
    esp_err_t err;

    err = rest_api_require_authenticated_status(req, &status);
    if (err != ESP_OK) {
        return err;
    }

    root = cJSON_CreateObject();
    network = cJSON_CreateObject();
    modio = cJSON_CreateObject();
    if ((root == NULL) || (network == NULL) || (modio == NULL)) {
        cJSON_Delete(root);
        cJSON_Delete(network);
        cJSON_Delete(modio);
        return rest_api_send_error(req, 500, "INTERNAL_ERROR", "Failed to allocate JSON response", true);
    }

    cJSON_AddNumberToObject(root, "uptime_seconds", (double)(esp_timer_get_time() / 1000000LL));
    cJSON_AddStringToObject(root, "firmware_version", esp_app_get_description()->version);
    cJSON_AddNumberToObject(root, "free_heap_bytes", (double)esp_get_free_heap_size());

    cJSON_AddStringToObject(network, "hostname", status.network.hostname);
    cJSON_AddBoolToObject(network, "connected", status.network.connected);
    cJSON_AddStringToObject(network, "ip", status.network.ip);
    cJSON_AddStringToObject(network, "netmask", status.network.netmask);
    cJSON_AddStringToObject(network, "gateway", status.network.gateway);
    cJSON_AddItemToObject(root, "network", network);

    cJSON_AddBoolToObject(modio, "present", status.modio_present);
    cJSON_AddStringToObject(modio, "sync", rest_api_modio_sync_to_string(status.modio_sync));
    cJSON_AddItemToObject(root, "modio", modio);
    return rest_api_send_json_response(req, 200, root, &status, true);
}

esp_err_t rest_api_send_error(httpd_req_t *req,
                              int http_status,
                              const char *code,
                              const char *message,
                              bool authenticated)
{
    rest_api_status_view_t status;
    cJSON *root = NULL;
    cJSON *error_obj = NULL;
    char *response = NULL;
    esp_err_t err;

    ESP_RETURN_ON_FALSE(req != NULL, ESP_ERR_INVALID_ARG, TAG, "HTTP request is required");
    ESP_RETURN_ON_FALSE(code != NULL, ESP_ERR_INVALID_ARG, TAG, "Error code is required");
    ESP_RETURN_ON_FALSE(message != NULL, ESP_ERR_INVALID_ARG, TAG, "Error message is required");

    root = cJSON_CreateObject();
    error_obj = cJSON_CreateObject();
    if ((root == NULL) || (error_obj == NULL)) {
        cJSON_Delete(root);
        cJSON_Delete(error_obj);
        httpd_resp_set_status(req, rest_api_http_status_text(500));
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req,
                                  "{\"error\":{\"code\":\"INTERNAL_ERROR\",\"message\":\"Failed to allocate JSON response\",\"status\":500}}");
    }

    cJSON_AddStringToObject(error_obj, "code", code);
    cJSON_AddStringToObject(error_obj, "message", message);
    cJSON_AddNumberToObject(error_obj, "status", http_status);
    cJSON_AddItemToObject(root, "error", error_obj);

    response = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (response == NULL) {
        httpd_resp_set_status(req, rest_api_http_status_text(500));
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req,
                                  "{\"error\":{\"code\":\"INTERNAL_ERROR\",\"message\":\"Failed to serialize JSON response\",\"status\":500}}");
    }

    httpd_resp_set_status(req, rest_api_http_status_text(http_status));
    httpd_resp_set_type(req, "application/json");

    if (authenticated && (http_status != 401) && (http_status != 403)) {
        err = rest_api_build_status_view(&status);
        if (err == ESP_OK) {
            rest_api_try_attach_device_context_headers(req, &status, true);
        }
    }

    err = httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    cJSON_free(response);
    return err;
}

bool rest_api_parse_id_from_uri(const char *uri, const char *prefix, uint32_t *out_id)
{
    const char *suffix;
    char *endptr = NULL;
    unsigned long parsed_id;

    if ((uri == NULL) || (prefix == NULL) || (out_id == NULL)) {
        return false;
    }

    if (strncmp(uri, prefix, strlen(prefix)) != 0) {
        return false;
    }

    suffix = uri + strlen(prefix);
    if (*suffix == '\0') {
        return false;
    }

    errno = 0;
    parsed_id = strtoul(suffix, &endptr, 10);
    if ((errno != 0) || (endptr == suffix) || (*endptr != '\0') || (parsed_id == 0UL) ||
            (parsed_id > UINT32_MAX)) {
        return false;
    }

    *out_id = (uint32_t)parsed_id;
    return true;
}

httpd_handle_t rest_api_get_server(void)
{
    return s_server;
}

esp_err_t rest_api_start(const rest_api_config_t *config)
{
    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    httpd_uri_t status_uri = {
        .uri = "/api/v1/status",
        .method = HTTP_GET,
        .handler = rest_api_status_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t relays_uri = {
        .uri = REST_API_RELAYS_URI,
        .method = HTTP_GET,
        .handler = rest_api_relays_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t onboard_relays_uri = {
        .uri = REST_API_ONBOARD_RELAYS_URI,
        .method = HTTP_GET,
        .handler = rest_api_onboard_relays_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t onboard_relay_set_uri = {
        .uri = "/api/v1/relays/onboard/*",
        .method = HTTP_PUT,
        .handler = rest_api_onboard_relay_set_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t onboard_relay_toggle_uri = {
        .uri = "/api/v1/relays/onboard/*/toggle",
        .method = HTTP_POST,
        .handler = rest_api_onboard_relay_toggle_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t modio_relays_uri = {
        .uri = REST_API_MODIO_RELAYS_URI,
        .method = HTTP_GET,
        .handler = rest_api_modio_relays_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t modio_relay_set_uri = {
        .uri = "/api/v1/relays/modio/*",
        .method = HTTP_PUT,
        .handler = rest_api_modio_relay_set_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t modio_relays_set_uri = {
        .uri = REST_API_MODIO_RELAYS_URI,
        .method = HTTP_PUT,
        .handler = rest_api_modio_relays_set_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t config_uri = {
        .uri = REST_API_CONFIG_URI,
        .method = HTTP_GET,
        .handler = rest_api_config_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t config_update_uri = {
        .uri = REST_API_CONFIG_URI,
        .method = HTTP_PUT,
        .handler = rest_api_config_update_handler,
        .user_ctx = NULL,
    };
    esp_err_t err;

    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "REST API config is required");
    ESP_RETURN_ON_FALSE(config->auth_handler != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "REST API auth handler is required");
    ESP_RETURN_ON_FALSE(config->status_provider != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "REST API status provider is required");
    ESP_RETURN_ON_FALSE(s_server == NULL, ESP_ERR_INVALID_STATE, TAG, "REST API server already started");

    err = esp_event_loop_create_default();
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return err;
    }

    server_config.server_port = (config->port != 0U) ? config->port : REST_API_DEFAULT_PORT;
    server_config.uri_match_fn = httpd_uri_match_wildcard;

    err = httpd_start(&s_server, &server_config);
    if (err != ESP_OK) {
        return err;
    }

    s_config = *config;
    err = httpd_register_uri_handler(s_server, &status_uri);
    if (err != ESP_OK) {
        httpd_stop(s_server);
        s_server = NULL;
        memset(&s_config, 0, sizeof(s_config));
        return err;
    }

    err = httpd_register_uri_handler(s_server, &relays_uri);
    if (err != ESP_OK) {
        httpd_stop(s_server);
        s_server = NULL;
        memset(&s_config, 0, sizeof(s_config));
        return err;
    }

    err = httpd_register_uri_handler(s_server, &onboard_relays_uri);
    if (err != ESP_OK) {
        httpd_stop(s_server);
        s_server = NULL;
        memset(&s_config, 0, sizeof(s_config));
        return err;
    }

    err = httpd_register_uri_handler(s_server, &onboard_relay_set_uri);
    if (err != ESP_OK) {
        httpd_stop(s_server);
        s_server = NULL;
        memset(&s_config, 0, sizeof(s_config));
        return err;
    }

    err = httpd_register_uri_handler(s_server, &onboard_relay_toggle_uri);
    if (err != ESP_OK) {
        httpd_stop(s_server);
        s_server = NULL;
        memset(&s_config, 0, sizeof(s_config));
        return err;
    }

    err = httpd_register_uri_handler(s_server, &modio_relays_uri);
    if (err != ESP_OK) {
        httpd_stop(s_server);
        s_server = NULL;
        memset(&s_config, 0, sizeof(s_config));
        return err;
    }

    err = httpd_register_uri_handler(s_server, &modio_relay_set_uri);
    if (err != ESP_OK) {
        httpd_stop(s_server);
        s_server = NULL;
        memset(&s_config, 0, sizeof(s_config));
        return err;
    }

    err = httpd_register_uri_handler(s_server, &modio_relays_set_uri);
    if (err != ESP_OK) {
        httpd_stop(s_server);
        s_server = NULL;
        memset(&s_config, 0, sizeof(s_config));
        return err;
    }

    err = httpd_register_uri_handler(s_server, &config_uri);
    if (err != ESP_OK) {
        httpd_stop(s_server);
        s_server = NULL;
        memset(&s_config, 0, sizeof(s_config));
        return err;
    }

    err = httpd_register_uri_handler(s_server, &config_update_uri);
    if (err != ESP_OK) {
        httpd_stop(s_server);
        s_server = NULL;
        memset(&s_config, 0, sizeof(s_config));
        return err;
    }

    ESP_LOGI(TAG, "REST API server started on port %u", (unsigned)server_config.server_port);
    return ESP_OK;
}

esp_err_t rest_api_stop(void)
{
    if (s_server == NULL) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(httpd_stop(s_server), TAG, "Failed to stop REST API server");
    s_server = NULL;
    memset(&s_config, 0, sizeof(s_config));

    return ESP_OK;
}

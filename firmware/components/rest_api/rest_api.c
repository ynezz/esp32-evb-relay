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
#include "relay.h"
#include "rest_api_events.h"

ESP_EVENT_DEFINE_BASE(EVB_RELAY_EVENT);

static const char *TAG = "rest_api";

static httpd_handle_t s_server;
static rest_api_config_t s_config;

static rest_api_auth_result_t rest_api_authorize_request(httpd_req_t *req);

#define REST_API_MAX_REQUEST_BODY_LEN 64U

static const char *REST_API_ONBOARD_RELAYS_URI = "/api/v1/relays/onboard";
static const char *REST_API_ONBOARD_RELAY_ID_PREFIX = "/api/v1/relays/onboard/";
static const char *REST_API_ONBOARD_RELAY_TOGGLE_SUFFIX = "/toggle";

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

static esp_err_t rest_api_build_status_view(rest_api_status_view_t *status)
{
    esp_err_t err;

    ESP_RETURN_ON_FALSE(status != NULL, ESP_ERR_INVALID_ARG, TAG, "Status output buffer is required");

    memset(status, 0, sizeof(*status));
    status->modio_present = false;
    status->modio_sync = REST_API_MODIO_SYNC_ABSENT;

    err = device_config_get_hostname(status->network.hostname, sizeof(status->network.hostname));
    if (err != ESP_OK) {
        return err;
    }

    if (s_config.status_provider != NULL) {
        err = s_config.status_provider(status, s_config.status_ctx);
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
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

    ESP_RETURN_ON_FALSE(req != NULL, ESP_ERR_INVALID_ARG, TAG, "HTTP request is required");
    ESP_RETURN_ON_FALSE(prefix != NULL, ESP_ERR_INVALID_ARG, TAG, "Relay URI prefix is required");
    ESP_RETURN_ON_FALSE(out_relay_id != NULL, ESP_ERR_INVALID_ARG, TAG, "Relay id output is required");

    parsed = (suffix == NULL) ? rest_api_parse_id_from_uri(req->uri, prefix, &parsed_id)
             : rest_api_parse_id_from_uri_with_suffix(req->uri, prefix, suffix, &parsed_id);
    if (!parsed || (parsed_id == 0U) || (parsed_id > RELAY_COUNT)) {
        return rest_api_send_error(req, 404, "RELAY_NOT_FOUND", "Relay not found", true);
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
    esp_err_t err;

    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "REST API config is required");
    ESP_RETURN_ON_FALSE(config->auth_handler != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "REST API auth handler is required");
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

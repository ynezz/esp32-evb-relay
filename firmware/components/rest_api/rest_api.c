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
#include "rest_api_events.h"

ESP_EVENT_DEFINE_BASE(EVB_RELAY_EVENT);

static const char *TAG = "rest_api";

static httpd_handle_t s_server;
static rest_api_config_t s_config;

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
    case REST_API_MODIO_SYNC_UNKNOWN:
        return "unknown";
    case REST_API_MODIO_SYNC_SYNCHRONIZED:
        return "synchronized";
    default:
        return "unknown";
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

static rest_api_auth_result_t rest_api_authorize_request(httpd_req_t *req)
{
    (void)req;

    if (s_config.auth_handler == NULL) {
        return REST_API_AUTH_RESULT_FORBIDDEN;
    }

    return s_config.auth_handler(req, s_config.auth_ctx);
}

static esp_err_t rest_api_status_handler(httpd_req_t *req)
{
    rest_api_auth_result_t auth_result;
    rest_api_status_view_t status;
    cJSON *root = NULL;
    cJSON *network = NULL;
    cJSON *modio = NULL;
    char *response = NULL;
    esp_err_t err;

    auth_result = rest_api_authorize_request(req);
    if (auth_result == REST_API_AUTH_RESULT_UNAUTHORIZED) {
        return rest_api_send_error(req, 401, "AUTH_REQUIRED", "Authentication required", false);
    }

    if (auth_result == REST_API_AUTH_RESULT_FORBIDDEN) {
        return rest_api_send_error(req, 403, "AUTH_FORBIDDEN", "Access denied", false);
    }

    err = rest_api_build_status_view(&status);
    if (err != ESP_OK) {
        return rest_api_send_error(req, 500, "STATUS_UNAVAILABLE", "Failed to gather status", true);
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

    response = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (response == NULL) {
        return rest_api_send_error(req, 500, "INTERNAL_ERROR", "Failed to serialize JSON response", true);
    }

    httpd_resp_set_status(req, rest_api_http_status_text(200));
    httpd_resp_set_type(req, "application/json");
    rest_api_try_attach_device_context_headers(req, &status, true);
    err = httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    cJSON_free(response);

    return err;
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

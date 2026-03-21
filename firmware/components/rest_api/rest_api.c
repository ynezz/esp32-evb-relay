#include "rest_api.h"

#include <inttypes.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "cJSON.h"
#include "device_config.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "input_monitor.h"
#include "lwip/sockets.h"
#include "mod_io.h"
#include "ota.h"
#include "relay.h"
#include "relay_events.h"
#include "rest_api_request_recv.h"
#include "rest_api_sse_lifetime.h"
#include "rest_api_wifi_config_update.h"

static const char *TAG = "rest_api";

static httpd_handle_t s_server;
static rest_api_config_t s_config;

static rest_api_auth_result_t rest_api_authorize_request(httpd_req_t *req);
static esp_err_t rest_api_send_error_with_status(httpd_req_t *req,
                                                 int http_status,
                                                 const char *code,
                                                 const char *message,
                                                 const rest_api_status_view_t *status,
                                                 bool authenticated);

#define REST_API_MAX_REQUEST_BODY_LEN 512U
#define REST_API_URI_HANDLER_COUNT 18U
#define REST_API_SSE_MAX_CLIENTS 4U
#define REST_API_SSE_DISPATCH_QUEUE_LENGTH 16U
#define REST_API_SSE_CLIENT_QUEUE_LENGTH 8U
#define REST_API_SSE_EVENT_NAME_MAX_LEN 24U
#define REST_API_SSE_DATA_MAX_LEN 128U
#define REST_API_SSE_CLIENT_TASK_STACK_WORDS 4096U
#define REST_API_SSE_CLIENT_TASK_PRIORITY 5U
#define REST_API_SSE_DISPATCH_TASK_STACK_WORDS 4096U
#define REST_API_SSE_DISPATCH_TASK_PRIORITY 6U
#define REST_API_SSE_CONNECTED_COMMENT ":connected\n\n"
#define REST_API_SSE_HEARTBEAT_COMMENT ":heartbeat\n\n"
#define REST_API_OTA_UPLOAD_CHUNK_LEN 1024U

#if defined(REST_API_ENABLE_TESTING_API)
#define REST_API_SSE_HEARTBEAT_MS 250U
#define REST_API_SSE_CLIENT_POLL_WAIT_MS 250U
#else
#define REST_API_SSE_HEARTBEAT_MS 30000U
#define REST_API_SSE_CLIENT_POLL_WAIT_MS 1000U
#endif

#define REST_API_SSE_KEEPALIVE_IDLE_SECONDS 10
#define REST_API_SSE_KEEPALIVE_INTERVAL_SECONDS 5
#define REST_API_SSE_KEEPALIVE_PROBE_COUNT 3

static const char *REST_API_RELAYS_URI = "/api/v1/relays";
static const char *REST_API_ONBOARD_RELAYS_URI = "/api/v1/relays/onboard";
static const char *REST_API_ONBOARD_RELAY_ID_PREFIX = "/api/v1/relays/onboard/";
static const char *REST_API_ONBOARD_RELAY_TOGGLE_SUFFIX = "/toggle";
static const char *REST_API_MODIO_RELAYS_URI = "/api/v1/relays/modio";
static const char *REST_API_MODIO_RELAY_ID_PREFIX = "/api/v1/relays/modio/";
static const char *REST_API_MODIO_RELAY_TOGGLE_SUFFIX = "/toggle";
static const char *REST_API_DIGITAL_INPUTS_URI = "/api/v1/inputs/digital";
static const char *REST_API_DIGITAL_INPUT_ID_PREFIX = "/api/v1/inputs/digital/";
static const char *REST_API_ANALOG_INPUTS_URI = "/api/v1/inputs/analog";
static const char *REST_API_ANALOG_INPUT_ID_PREFIX = "/api/v1/inputs/analog/";
static const char *REST_API_EVENTS_URI = "/api/v1/events";
static const char *REST_API_OTA_URI = "/api/v1/ota";
static const char *REST_API_CONFIG_URI = "/api/v1/config";
static const char *REST_API_CONFIG_WIFI_URI = "/api/v1/config/wifi";

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

typedef struct {
    char event[REST_API_SSE_EVENT_NAME_MAX_LEN];
    char data[REST_API_SSE_DATA_MAX_LEN];
} rest_api_sse_message_t;

typedef struct {
    bool started;
    QueueHandle_t dispatch_queue;
    SemaphoreHandle_t lock;
    TaskHandle_t dispatch_task;
    esp_event_handler_instance_t event_handler;
    rest_api_sse_client_t clients[REST_API_SSE_MAX_CLIENTS];
} rest_api_sse_state_t;

static rest_api_sse_state_t s_sse_state;
static bool rest_api_sse_lock(void);
static void rest_api_sse_unlock(void);

#if defined(REST_API_ENABLE_TESTING_API)
typedef struct {
    volatile bool hold_dispatch_task_on_shutdown;
    volatile bool dispatch_shutdown_reached;
    volatile bool dispatch_task_deleted_by_stop;
    volatile bool force_next_client_task_create_failure;
} rest_api_testing_state_t;

static rest_api_testing_state_t s_testing_state;

void rest_api_sse_hold_dispatch_task_on_shutdown_for_testing(bool hold)
{
    s_testing_state.hold_dispatch_task_on_shutdown = hold;
}

bool rest_api_sse_wait_for_dispatch_shutdown_reached_for_testing(uint32_t timeout_ms)
{
    TickType_t start_tick = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    while (!s_testing_state.dispatch_shutdown_reached) {
        if ((timeout_ticks == 0U) || ((xTaskGetTickCount() - start_tick) >= timeout_ticks)) {
            return false;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }

    return true;
}

bool rest_api_sse_dispatch_task_deleted_by_stop_for_testing(void)
{
    return s_testing_state.dispatch_task_deleted_by_stop;
}

void rest_api_sse_force_next_client_task_create_failure_for_testing(void)
{
    s_testing_state.force_next_client_task_create_failure = true;
}

size_t rest_api_sse_active_client_count_for_testing(void)
{
    size_t active_client_count = 0U;

    if (!rest_api_sse_lock()) {
        return 0U;
    }

    for (size_t index = 0; index < REST_API_SSE_MAX_CLIENTS; ++index) {
        if (s_sse_state.clients[index].active) {
            ++active_client_count;
        }
    }

    rest_api_sse_unlock();
    return active_client_count;
}

bool rest_api_sse_wait_for_active_client_count_for_testing(size_t expected_count, uint32_t timeout_ms)
{
    TickType_t start_tick = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    while (rest_api_sse_active_client_count_for_testing() != expected_count) {
        if ((timeout_ticks == 0U) || ((xTaskGetTickCount() - start_tick) >= timeout_ticks)) {
            return false;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }

    return true;
}
#endif

static esp_err_t rest_api_send_error_with_retryable(httpd_req_t *req,
                                                    int http_status,
                                                    const char *code,
                                                    const char *message,
                                                    bool authenticated,
                                                    bool retryable);
static esp_err_t rest_api_send_request_body_read_error(httpd_req_t *req, esp_err_t err);
static bool rest_api_sse_lock(void);
static void rest_api_sse_unlock(void);
static esp_err_t rest_api_sse_start(void);
static void rest_api_sse_stop(void);
static void rest_api_sse_complete_async_request(httpd_req_t *req);

#if CONFIG_ESP_TASK_WDT_EN
static bool rest_api_task_watchdog_register(const char *task_name)
{
    esp_err_t err = esp_task_wdt_status(NULL);

    if (err == ESP_OK) {
        return true;
    }

    if (err == ESP_ERR_NOT_FOUND) {
        err = esp_task_wdt_add(NULL);
        if (err == ESP_OK) {
            return true;
        }
    }

    if (err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Failed to register %s with task watchdog: %s", task_name, esp_err_to_name(err));
    }

    return false;
}

static bool rest_api_task_watchdog_reset(bool registered, const char *task_name)
{
    esp_err_t err;

    if (!registered) {
        return false;
    }

    err = esp_task_wdt_reset();
    if (err == ESP_OK) {
        return true;
    }

    if ((err != ESP_ERR_INVALID_STATE) && (err != ESP_ERR_NOT_FOUND)) {
        ESP_LOGW(TAG, "Failed to feed %s task watchdog: %s", task_name, esp_err_to_name(err));
    }

    return false;
}

static void rest_api_task_watchdog_delete(TaskHandle_t task_handle, const char *task_name)
{
    esp_err_t err = esp_task_wdt_delete(task_handle);

    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE) && (err != ESP_ERR_NOT_FOUND)) {
        ESP_LOGW(TAG, "Failed to unregister %s from task watchdog: %s",
                 task_name,
                 esp_err_to_name(err));
    }
}
#else
static bool rest_api_task_watchdog_register(const char *task_name)
{
    (void)task_name;
    return false;
}

static bool rest_api_task_watchdog_reset(bool registered, const char *task_name)
{
    (void)registered;
    (void)task_name;
    return false;
}

static void rest_api_task_watchdog_delete(TaskHandle_t task_handle, const char *task_name)
{
    (void)task_handle;
    (void)task_name;
}
#endif

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
    case 413:
        return "413 Payload Too Large";
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
    ESP_RETURN_ON_FALSE(status != NULL, ESP_ERR_INVALID_ARG, TAG, "Status output buffer is required");
    ESP_RETURN_ON_FALSE(s_config.status_provider != NULL,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "Status provider is required");

    memset(status, 0, sizeof(*status));
    return s_config.status_provider(status, s_config.status_ctx);
}

static bool rest_api_require_authenticated_status(httpd_req_t *req,
                                                  rest_api_status_view_t *out_status,
                                                  esp_err_t *out_err)
{
    rest_api_auth_result_t auth_result;
    esp_err_t err;

    if ((req == NULL) || (out_status == NULL) || (out_err == NULL)) {
        if (out_err != NULL) {
            *out_err = ESP_ERR_INVALID_ARG;
        }
        ESP_LOGE(TAG, "HTTP request, status output, and error output are required");
        return false;
    }

    auth_result = rest_api_authorize_request(req);
    if (auth_result == REST_API_AUTH_RESULT_UNAUTHORIZED) {
        *out_err = rest_api_send_error(req, 401, "AUTH_REQUIRED", "Authentication required", false);
        return false;
    }

    if (auth_result == REST_API_AUTH_RESULT_FORBIDDEN) {
        *out_err = rest_api_send_error(req, 403, "AUTH_FORBIDDEN", "Access denied", false);
        return false;
    }

    err = rest_api_build_status_view(out_status);
    if (err != ESP_OK) {
        *out_err = rest_api_send_error(req, 500, "STATUS_UNAVAILABLE", "Failed to gather status", true);
        return false;
    }

    *out_err = ESP_OK;
    return true;
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

static esp_err_t rest_api_parse_modio_relay_id(httpd_req_t *req,
                                               const char *suffix,
                                               uint8_t *out_relay_id)
{
    uint32_t parsed_id = 0;
    bool parsed;
    esp_err_t err;

    ESP_RETURN_ON_FALSE(req != NULL, ESP_ERR_INVALID_ARG, TAG, "HTTP request is required");
    ESP_RETURN_ON_FALSE(out_relay_id != NULL, ESP_ERR_INVALID_ARG, TAG, "Relay id output is required");

    parsed = (suffix == NULL) ? rest_api_parse_id_from_uri(req->uri,
                                                           REST_API_MODIO_RELAY_ID_PREFIX,
                                                           &parsed_id)
             : rest_api_parse_id_from_uri_with_suffix(req->uri,
                                                      REST_API_MODIO_RELAY_ID_PREFIX,
                                                      suffix,
                                                      &parsed_id);
    if (!parsed || (parsed_id == 0U) || (parsed_id > MOD_IO_RELAY_COUNT)) {
        err = rest_api_send_error(req, 404, "RELAY_NOT_FOUND", "Relay not found", true);
        if (err != ESP_OK) {
            return err;
        }
        return ESP_ERR_NOT_FOUND;
    }

    *out_relay_id = (uint8_t)parsed_id;
    return ESP_OK;
}

static esp_err_t rest_api_parse_input_id(httpd_req_t *req,
                                         const char *prefix,
                                         uint8_t max_input_id,
                                         uint8_t *out_input_id)
{
    uint32_t parsed_id = 0;
    esp_err_t err;

    ESP_RETURN_ON_FALSE(req != NULL, ESP_ERR_INVALID_ARG, TAG, "HTTP request is required");
    ESP_RETURN_ON_FALSE(prefix != NULL, ESP_ERR_INVALID_ARG, TAG, "Input URI prefix is required");
    ESP_RETURN_ON_FALSE(out_input_id != NULL, ESP_ERR_INVALID_ARG, TAG, "Input id output is required");

    if (!rest_api_parse_id_from_uri(req->uri, prefix, &parsed_id) || (parsed_id == 0U) ||
            (parsed_id > max_input_id)) {
        err = rest_api_send_error(req, 404, "INPUT_NOT_FOUND", "Input not found", true);
        if (err != ESP_OK) {
            return err;
        }
        return ESP_ERR_NOT_FOUND;
    }

    *out_input_id = (uint8_t)parsed_id;
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

static cJSON *rest_api_create_digital_input_object(uint8_t input_id, bool state)
{
    cJSON *input = cJSON_CreateObject();

    if (input == NULL) {
        return NULL;
    }

    cJSON_AddNumberToObject(input, "id", input_id);
    cJSON_AddBoolToObject(input, "state", state);
    return input;
}

static cJSON *rest_api_create_analog_input_object(uint8_t input_id, uint16_t value)
{
    cJSON *input = cJSON_CreateObject();

    if (input == NULL) {
        return NULL;
    }

    cJSON_AddNumberToObject(input, "id", input_id);
    cJSON_AddNumberToObject(input, "value", value);
    return input;
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

static esp_err_t rest_api_append_digital_input_objects(cJSON *inputs, uint8_t digital_mask)
{
    ESP_RETURN_ON_FALSE(inputs != NULL, ESP_ERR_INVALID_ARG, TAG, "Input array is required");

    for (uint8_t input_id = 1U; input_id <= MOD_IO_DIGITAL_INPUT_COUNT; ++input_id) {
        cJSON *input = NULL;
        bool state = (digital_mask & (uint8_t)(1U << (input_id - 1U))) != 0U;

        input = rest_api_create_digital_input_object(input_id, state);
        if (input == NULL) {
            return ESP_ERR_NO_MEM;
        }

        cJSON_AddItemToArray(inputs, input);
    }

    return ESP_OK;
}

static esp_err_t rest_api_append_analog_input_objects(
    cJSON *inputs,
    const uint16_t analog_values[MOD_IO_ANALOG_INPUT_COUNT])
{
    ESP_RETURN_ON_FALSE(inputs != NULL, ESP_ERR_INVALID_ARG, TAG, "Input array is required");
    ESP_RETURN_ON_FALSE(analog_values != NULL, ESP_ERR_INVALID_ARG, TAG, "Analog values are required");

    for (uint8_t input_id = 1U; input_id <= MOD_IO_ANALOG_INPUT_COUNT; ++input_id) {
        cJSON *input = NULL;

        input = rest_api_create_analog_input_object(input_id, analog_values[input_id - 1U]);
        if (input == NULL) {
            return ESP_ERR_NO_MEM;
        }

        cJSON_AddItemToArray(inputs, input);
    }

    return ESP_OK;
}

static uint64_t rest_api_timestamp_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000LL);
}

static void rest_api_add_sample_metadata(cJSON *root,
                                         uint64_t sample_ts_ms,
                                         uint64_t staleness_ms,
                                         uint32_t poll_interval_ms)
{
    if (root == NULL) {
        return;
    }

    cJSON_AddNumberToObject(root, "sample_ts_ms", (double)sample_ts_ms);
    cJSON_AddNumberToObject(root, "staleness_ms", (double)staleness_ms);
    cJSON_AddNumberToObject(root, "poll_interval_ms", (double)poll_interval_ms);
}

static bool rest_api_sse_lock(void)
{
    return (s_sse_state.lock != NULL) && (xSemaphoreTake(s_sse_state.lock, portMAX_DELAY) == pdTRUE);
}

static void rest_api_sse_unlock(void)
{
    if (s_sse_state.lock != NULL) {
        xSemaphoreGive(s_sse_state.lock);
    }
}

static const char *rest_api_sse_group_name(evb_relay_relay_group_t group)
{
    switch (group) {
    case EVB_RELAY_RELAY_GROUP_ONBOARD:
        return "onboard";
    case EVB_RELAY_RELAY_GROUP_MODIO:
        return "modio";
    default:
        return "unknown";
    }
}

static bool rest_api_sse_format_message(int32_t event_id,
                                        const void *event_data,
                                        rest_api_sse_message_t *out_message)
{
    int written = 0;

    if ((event_data == NULL) || (out_message == NULL)) {
        return false;
    }

    memset(out_message, 0, sizeof(*out_message));

    switch (event_id) {
    case EVB_RELAY_EVENT_DIGITAL_INPUT: {
        const evb_relay_digital_input_event_t *event = event_data;

        strncpy(out_message->event, "digital_input", sizeof(out_message->event) - 1U);
        written = snprintf(out_message->data,
                           sizeof(out_message->data),
                           "{\"id\":%u,\"state\":%s,\"ts_ms\":%" PRIu64 "}",
                           (unsigned)event->id,
                           event->state ? "true" : "false",
                           event->ts_ms);
        break;
    }
    case EVB_RELAY_EVENT_ANALOG_INPUT: {
        const evb_relay_analog_input_event_t *event = event_data;

        strncpy(out_message->event, "analog_input", sizeof(out_message->event) - 1U);
        written = snprintf(out_message->data,
                           sizeof(out_message->data),
                           "{\"id\":%u,\"value\":%u,\"ts_ms\":%" PRIu64 "}",
                           (unsigned)event->id,
                           (unsigned)event->value,
                           event->ts_ms);
        break;
    }
    case EVB_RELAY_EVENT_RELAY_CHANGED: {
        const evb_relay_relay_changed_event_t *event = event_data;

        strncpy(out_message->event, "relay_changed", sizeof(out_message->event) - 1U);
        written = snprintf(out_message->data,
                           sizeof(out_message->data),
                           "{\"group\":\"%s\",\"id\":%u,\"state\":%s,\"ts_ms\":%" PRIu64 "}",
                           rest_api_sse_group_name(event->group),
                           (unsigned)event->id,
                           event->state ? "true" : "false",
                           event->ts_ms);
        break;
    }
    case EVB_RELAY_EVENT_BUTTON: {
        const evb_relay_button_event_t *event = event_data;

        strncpy(out_message->event, "button", sizeof(out_message->event) - 1U);
        written = snprintf(out_message->data,
                           sizeof(out_message->data),
                           "{\"pressed\":%s,\"ts_ms\":%" PRIu64 "}",
                           event->pressed ? "true" : "false",
                           event->ts_ms);
        break;
    }
    case EVB_RELAY_EVENT_MODIO_PRESENCE: {
        const evb_relay_modio_presence_event_t *event = event_data;

        strncpy(out_message->event, "modio_presence", sizeof(out_message->event) - 1U);
        written = snprintf(out_message->data,
                           sizeof(out_message->data),
                           "{\"present\":%s,\"ts_ms\":%" PRIu64 "}",
                           event->present ? "true" : "false",
                           event->ts_ms);
        break;
    }
    default:
        return false;
    }

    return (written > 0) && ((size_t)written < sizeof(out_message->data));
}

static void rest_api_sse_release_client_slot(rest_api_sse_client_t *client)
{
    static const rest_api_sse_lifetime_hooks_t hooks = {
        .lock = rest_api_sse_lock,
        .unlock = rest_api_sse_unlock,
        .delete_queue = vQueueDelete,
        .complete_async_request = rest_api_sse_complete_async_request,
    };

    rest_api_sse_release_client_lifetime(client, &hooks);
}

static void rest_api_sse_release_disconnected_client_slot(rest_api_sse_client_t *client)
{
    /* Keep the client visible until async completion returns so shutdown can
     * still force-release the task if completion stalls. */
    rest_api_sse_release_client_slot(client);
}

static void rest_api_sse_force_release_client_slot(rest_api_sse_client_t *client)
{
    static const rest_api_sse_lifetime_hooks_t hooks = {
        .lock = rest_api_sse_lock,
        .unlock = rest_api_sse_unlock,
        .delete_queue = vQueueDelete,
        .complete_async_request = rest_api_sse_complete_async_request,
        .delete_task = vTaskDelete,
        .current_task_handle = xTaskGetCurrentTaskHandle,
    };

    rest_api_sse_force_release_client_lifetime(client, &hooks);
}

static void rest_api_sse_complete_async_request(httpd_req_t *req)
{
    if (req != NULL) {
        (void)httpd_req_async_handler_complete(req);
    }
}

static void rest_api_sse_release_startup_client_slot(rest_api_sse_client_t *client, httpd_req_t *req)
{
    static const rest_api_sse_lifetime_hooks_t hooks = {
        .lock = rest_api_sse_lock,
        .unlock = rest_api_sse_unlock,
        .delete_queue = vQueueDelete,
        .complete_async_request = rest_api_sse_complete_async_request,
    };

    rest_api_sse_release_startup_client_lifetime(client, req, &hooks);
}

static esp_err_t rest_api_sse_send_async_error_and_complete(httpd_req_t *async_req,
                                                            int http_status,
                                                            const char *code,
                                                            const char *message)
{
    esp_err_t err;

    ESP_RETURN_ON_FALSE(async_req != NULL, ESP_ERR_INVALID_ARG, TAG, "Async request is required");

    err = rest_api_send_error(async_req, http_status, code, message, true);
    (void)httpd_req_async_handler_complete(async_req);
    return err;
}

static bool rest_api_sse_socket_probe_error_is_retryable(int socket_errno)
{
    return (socket_errno == EAGAIN) || (socket_errno == EWOULDBLOCK) || (socket_errno == EINTR);
}

static void rest_api_sse_configure_socket_keepalive(int sockfd)
{
    int enabled = 1;
    int idle_seconds = REST_API_SSE_KEEPALIVE_IDLE_SECONDS;
    int interval_seconds = REST_API_SSE_KEEPALIVE_INTERVAL_SECONDS;
    int probe_count = REST_API_SSE_KEEPALIVE_PROBE_COUNT;

    if (sockfd < 0) {
        return;
    }

    if (setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled)) < 0) {
        ESP_LOGW(TAG, "Failed to enable SSE socket keepalive: errno=%d", errno);
        return;
    }

    if (setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPIDLE, &idle_seconds, sizeof(idle_seconds)) < 0) {
        ESP_LOGW(TAG, "Failed to set SSE keepalive idle: errno=%d", errno);
    }

    if (setsockopt(sockfd,
                   IPPROTO_TCP,
                   TCP_KEEPINTVL,
                   &interval_seconds,
                   sizeof(interval_seconds)) < 0) {
        ESP_LOGW(TAG, "Failed to set SSE keepalive interval: errno=%d", errno);
    }

    if (setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPCNT, &probe_count, sizeof(probe_count)) < 0) {
        ESP_LOGW(TAG, "Failed to set SSE keepalive probe count: errno=%d", errno);
    }
}

static bool rest_api_sse_client_connection_alive(const rest_api_sse_client_t *client)
{
    uint8_t probe_byte = 0U;
    ssize_t probe_result;

    if ((client == NULL) || (client->sockfd < 0)) {
        return false;
    }

    errno = 0;
    probe_result = recv(client->sockfd,
                        &probe_byte,
                        sizeof(probe_byte),
                        MSG_PEEK | MSG_DONTWAIT);
    if (probe_result > 0) {
        return true;
    }

    if (probe_result == 0) {
        return false;
    }

    return rest_api_sse_socket_probe_error_is_retryable(errno);
}

static void rest_api_sse_dispatch_event_handler(void *arg,
                                                esp_event_base_t event_base,
                                                int32_t event_id,
                                                void *event_data)
{
    rest_api_sse_message_t message;

    (void)arg;

    if ((event_base != EVB_RELAY_EVENT) || !s_sse_state.started || (s_sse_state.dispatch_queue == NULL)) {
        return;
    }

    if (!rest_api_sse_format_message(event_id, event_data, &message)) {
        return;
    }

    if (xQueueSend(s_sse_state.dispatch_queue, &message, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Dropping SSE event because the dispatch queue is full");
    }
}

static void rest_api_sse_dispatch_task(void *arg)
{
    rest_api_sse_message_t message;
    bool watchdog_registered;

    (void)arg;
    watchdog_registered = rest_api_task_watchdog_register("rest_api_sse_dispatch");

    while (s_sse_state.started) {
        if ((s_sse_state.dispatch_queue == NULL) ||
                (xQueueReceive(s_sse_state.dispatch_queue,
                               &message,
                               pdMS_TO_TICKS(REST_API_SSE_CLIENT_POLL_WAIT_MS)) != pdTRUE)) {
            watchdog_registered =
                rest_api_task_watchdog_reset(watchdog_registered, "rest_api_sse_dispatch");
            continue;
        }

        if (!rest_api_sse_lock()) {
            continue;
        }

        for (size_t index = 0; index < REST_API_SSE_MAX_CLIENTS; ++index) {
            rest_api_sse_client_t *client = &s_sse_state.clients[index];

            if (!client->active || client->close_requested || (client->queue == NULL)) {
                continue;
            }

            if (xQueueSend(client->queue, &message, 0) != pdTRUE) {
                client->close_requested = true;
                if ((s_server != NULL) && (client->sockfd >= 0)) {
                    (void)httpd_sess_trigger_close(s_server, client->sockfd);
                }
            }
        }

        rest_api_sse_unlock();
        watchdog_registered =
            rest_api_task_watchdog_reset(watchdog_registered, "rest_api_sse_dispatch");
    }

    rest_api_task_watchdog_delete(NULL, "rest_api_sse_dispatch");

#if defined(REST_API_ENABLE_TESTING_API)
    s_testing_state.dispatch_shutdown_reached = true;
    while (s_testing_state.hold_dispatch_task_on_shutdown) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
#endif

    /* rest_api_sse_stop() is the sole owner of deleting this task. */
    vTaskSuspend(NULL);
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

static void rest_api_sse_client_task(void *arg)
{
    rest_api_sse_client_t *client = arg;
    TickType_t last_send_tick = xTaskGetTickCount();
    esp_err_t err = ESP_OK;

    if ((client == NULL) || (client->req == NULL) || (client->queue == NULL)) {
        vTaskDelete(NULL);
        return;
    }

    httpd_resp_set_type(client->req, "text/event-stream");
    httpd_resp_set_hdr(client->req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(client->req, "Connection", "keep-alive");
    httpd_resp_set_hdr(client->req, "X-Accel-Buffering", "no");
    rest_api_try_attach_device_context_headers(client->req, &client->status, true);

    err = httpd_resp_send_chunk(client->req,
                                REST_API_SSE_CONNECTED_COMMENT,
                                HTTPD_RESP_USE_STRLEN);
    if (err == ESP_OK) {
        last_send_tick = xTaskGetTickCount();
    }

    while ((err == ESP_OK) && !client->close_requested) {
        rest_api_sse_message_t message;

        if (xQueueReceive(client->queue,
                          &message,
                          pdMS_TO_TICKS(REST_API_SSE_CLIENT_POLL_WAIT_MS)) == pdTRUE) {
            char chunk[REST_API_SSE_EVENT_NAME_MAX_LEN + REST_API_SSE_DATA_MAX_LEN + 32U];
            int written = snprintf(chunk,
                                   sizeof(chunk),
                                   "event: %s\ndata: %s\n\n",
                                   message.event,
                                   message.data);

            if ((written <= 0) || ((size_t)written >= sizeof(chunk))) {
                err = ESP_FAIL;
                break;
            }

            err = httpd_resp_send_chunk(client->req, chunk, HTTPD_RESP_USE_STRLEN);
            last_send_tick = xTaskGetTickCount();
            continue;
        }

        if (client->close_requested) {
            break;
        }

        if (!rest_api_sse_client_connection_alive(client)) {
            err = ESP_FAIL;
            break;
        }

        if ((xTaskGetTickCount() - last_send_tick) < pdMS_TO_TICKS(REST_API_SSE_HEARTBEAT_MS)) {
            continue;
        }

        err = httpd_resp_send_chunk(client->req,
                                    REST_API_SSE_HEARTBEAT_COMMENT,
                                    HTTPD_RESP_USE_STRLEN);
        last_send_tick = xTaskGetTickCount();
    }

    if (err == ESP_OK) {
        rest_api_sse_release_client_slot(client);
    } else {
        rest_api_sse_release_disconnected_client_slot(client);
    }
    vTaskDelete(NULL);
}

static esp_err_t rest_api_events_handler(httpd_req_t *req)
{
    rest_api_status_view_t status;
    rest_api_sse_client_t *client = NULL;
    httpd_req_t *async_req = NULL;
    BaseType_t task_result;
    esp_err_t err;

    if (!rest_api_require_authenticated_status(req, &status, &err)) {
        return err;
    }

    if (!s_sse_state.started) {
        return rest_api_send_error(req,
                                   503,
                                   "SSE_UNAVAILABLE",
                                   "Event stream is not available",
                                   true);
    }

    err = httpd_req_async_handler_begin(req, &async_req);
    if (err != ESP_OK) {
        return rest_api_send_error(req,
                                   503,
                                   "SSE_UNAVAILABLE",
                                   "Failed to open event stream",
                                   true);
    }

    if (!rest_api_sse_lock()) {
        return rest_api_sse_send_async_error_and_complete(async_req,
                                                          503,
                                                          "SSE_UNAVAILABLE",
                                                          "Event stream is not available");
    }

    for (size_t index = 0; index < REST_API_SSE_MAX_CLIENTS; ++index) {
        if (!s_sse_state.clients[index].active) {
            client = &s_sse_state.clients[index];
            rest_api_sse_reset_client(client);
            client->active = true;
            client->status = status;
            break;
        }
    }
    rest_api_sse_unlock();

    if (client == NULL) {
        return rest_api_sse_send_async_error_and_complete(async_req,
                                                          503,
                                                          "SSE_CLIENT_LIMIT_REACHED",
                                                          "Too many SSE clients are connected");
    }

    client->queue = xQueueCreate(REST_API_SSE_CLIENT_QUEUE_LENGTH, sizeof(rest_api_sse_message_t));
    if (client->queue == NULL) {
        err = rest_api_send_error(async_req,
                                  503,
                                  "SSE_UNAVAILABLE",
                                  "Failed to allocate event stream buffers",
                                  true);
        rest_api_sse_release_startup_client_slot(client, async_req);
        return err;
    }

    client->req = async_req;
    client->sockfd = httpd_req_to_sockfd(async_req);
    rest_api_sse_configure_socket_keepalive(client->sockfd);

#if defined(REST_API_ENABLE_TESTING_API)
    if (s_testing_state.force_next_client_task_create_failure) {
        s_testing_state.force_next_client_task_create_failure = false;
        task_result = pdFAIL;
    } else
#endif
    {
        task_result = xTaskCreate(rest_api_sse_client_task,
                                  "rest_api_sse",
                                  REST_API_SSE_CLIENT_TASK_STACK_WORDS,
                                  client,
                                  REST_API_SSE_CLIENT_TASK_PRIORITY,
                                  &client->task_handle);
    }

    if (task_result != pdPASS) {
        err = rest_api_send_error(async_req,
                                  503,
                                  "SSE_UNAVAILABLE",
                                  "Failed to start event stream task",
                                  true);
        rest_api_sse_release_startup_client_slot(client, async_req);
        return err;
    }

    return ESP_OK;
}

static esp_err_t rest_api_sse_start(void)
{
    BaseType_t task_result;
    esp_err_t err;

    if (s_sse_state.started) {
        return ESP_OK;
    }

    memset(&s_sse_state, 0, sizeof(s_sse_state));

#if defined(REST_API_ENABLE_TESTING_API)
    s_testing_state.hold_dispatch_task_on_shutdown = false;
    s_testing_state.dispatch_shutdown_reached = false;
    s_testing_state.dispatch_task_deleted_by_stop = false;
    s_testing_state.force_next_client_task_create_failure = false;
#endif

    s_sse_state.lock = xSemaphoreCreateMutex();
    if (s_sse_state.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_sse_state.dispatch_queue = xQueueCreate(REST_API_SSE_DISPATCH_QUEUE_LENGTH,
                                              sizeof(rest_api_sse_message_t));
    if (s_sse_state.dispatch_queue == NULL) {
        vSemaphoreDelete(s_sse_state.lock);
        s_sse_state.lock = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_sse_state.started = true;
    task_result = xTaskCreate(rest_api_sse_dispatch_task,
                              "rest_api_sse_dispatch",
                              REST_API_SSE_DISPATCH_TASK_STACK_WORDS,
                              NULL,
                              REST_API_SSE_DISPATCH_TASK_PRIORITY,
                              &s_sse_state.dispatch_task);
    if (task_result != pdPASS) {
        s_sse_state.started = false;
        vQueueDelete(s_sse_state.dispatch_queue);
        vSemaphoreDelete(s_sse_state.lock);
        s_sse_state.dispatch_queue = NULL;
        s_sse_state.lock = NULL;
        return ESP_ERR_NO_MEM;
    }

    err = esp_event_handler_instance_register(EVB_RELAY_EVENT,
                                              ESP_EVENT_ANY_ID,
                                              rest_api_sse_dispatch_event_handler,
                                              NULL,
                                              &s_sse_state.event_handler);
    if (err != ESP_OK) {
        rest_api_sse_stop();
        return err;
    }

    return ESP_OK;
}

static void rest_api_sse_stop(void)
{
    if (!s_sse_state.started && (s_sse_state.lock == NULL) &&
            (s_sse_state.dispatch_queue == NULL) && (s_sse_state.dispatch_task == NULL)) {
        return;
    }

    if (s_sse_state.event_handler != NULL) {
        (void)esp_event_handler_instance_unregister(EVB_RELAY_EVENT,
                                                    ESP_EVENT_ANY_ID,
                                                    s_sse_state.event_handler);
        s_sse_state.event_handler = NULL;
    }

    s_sse_state.started = false;

    if (rest_api_sse_lock()) {
        for (size_t index = 0; index < REST_API_SSE_MAX_CLIENTS; ++index) {
            rest_api_sse_client_t *client = &s_sse_state.clients[index];

            if (!client->active) {
                continue;
            }

            client->close_requested = true;
            if ((s_server != NULL) && (client->sockfd >= 0)) {
                (void)httpd_sess_trigger_close(s_server, client->sockfd);
            }
        }
        rest_api_sse_unlock();
    }

    for (uint8_t attempt = 0U; attempt < 15U; ++attempt) {
        bool any_active = false;

        if (rest_api_sse_lock()) {
            for (size_t index = 0; index < REST_API_SSE_MAX_CLIENTS; ++index) {
                if (s_sse_state.clients[index].active) {
                    any_active = true;
                    break;
                }
            }
            rest_api_sse_unlock();
        }

        if (!any_active) {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(REST_API_SSE_CLIENT_POLL_WAIT_MS));
    }

    for (size_t index = 0; index < REST_API_SSE_MAX_CLIENTS; ++index) {
        rest_api_sse_force_release_client_slot(&s_sse_state.clients[index]);
    }

#if defined(REST_API_ENABLE_TESTING_API)
    if (s_testing_state.hold_dispatch_task_on_shutdown) {
        (void)rest_api_sse_wait_for_dispatch_shutdown_reached_for_testing(
            REST_API_SSE_CLIENT_POLL_WAIT_MS + 1000U);
    }
#endif

    if (s_sse_state.dispatch_task != NULL) {
#if defined(REST_API_ENABLE_TESTING_API)
        s_testing_state.dispatch_task_deleted_by_stop = true;
#endif
        rest_api_task_watchdog_delete(s_sse_state.dispatch_task, "rest_api_sse_dispatch");
        vTaskDelete(s_sse_state.dispatch_task);
        s_sse_state.dispatch_task = NULL;
    }

    if (s_sse_state.dispatch_queue != NULL) {
        vQueueDelete(s_sse_state.dispatch_queue);
        s_sse_state.dispatch_queue = NULL;
    }

    if (s_sse_state.lock != NULL) {
        vSemaphoreDelete(s_sse_state.lock);
        s_sse_state.lock = NULL;
    }

    memset(s_sse_state.clients, 0, sizeof(s_sse_state.clients));

#if defined(REST_API_ENABLE_TESTING_API)
    s_testing_state.hold_dispatch_task_on_shutdown = false;
    s_testing_state.force_next_client_task_create_failure = false;
#endif
}

static esp_err_t rest_api_read_request_body(httpd_req_t *req,
                                            char *buffer,
                                            size_t buffer_size,
                                            size_t *out_len)
{
    ESP_RETURN_ON_FALSE(req != NULL, ESP_ERR_INVALID_ARG, TAG, "HTTP request is required");
    ESP_RETURN_ON_FALSE(buffer != NULL, ESP_ERR_INVALID_ARG, TAG, "Request body buffer is required");
    ESP_RETURN_ON_FALSE(buffer_size > 1U, ESP_ERR_INVALID_ARG, TAG, "Request body buffer is too small");

    if ((req->content_len <= 0) || ((size_t)req->content_len >= buffer_size)) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = rest_api_request_recv_exact(req, buffer, (size_t)req->content_len, out_len);

    if (err != ESP_OK) {
        return err;
    }

    buffer[req->content_len] = '\0';
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
    if (err != ESP_OK) {
        return rest_api_send_request_body_read_error(req, err);
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
    if (err != ESP_OK) {
        return rest_api_send_request_body_read_error(req, err);
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

static esp_err_t rest_api_prepare_input_snapshot(httpd_req_t *req,
                                                 rest_api_status_view_t *out_status,
                                                 input_monitor_snapshot_t *out_snapshot,
                                                 uint32_t *out_poll_interval_ms,
                                                 uint64_t *out_staleness_ms)
{
    esp_err_t err;

    ESP_RETURN_ON_FALSE(req != NULL, ESP_ERR_INVALID_ARG, TAG, "HTTP request is required");
    ESP_RETURN_ON_FALSE(out_status != NULL, ESP_ERR_INVALID_ARG, TAG, "Status output is required");
    ESP_RETURN_ON_FALSE(out_snapshot != NULL, ESP_ERR_INVALID_ARG, TAG, "Input snapshot is required");
    ESP_RETURN_ON_FALSE(out_poll_interval_ms != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "Poll interval output is required");
    ESP_RETURN_ON_FALSE(out_staleness_ms != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "Staleness output is required");

    if (!rest_api_require_authenticated_status(req, out_status, &err)) {
        return err;
    }

    err = input_monitor_get_snapshot(out_snapshot);
    if (err == ESP_ERR_INVALID_STATE) {
        if (!out_status->modio_present) {
            out_status->modio_sync = REST_API_MODIO_SYNC_ABSENT;
            err = rest_api_send_error(req, 503, "MODIO_NOT_PRESENT", "MOD-IO is not present", true);
            if (err != ESP_OK) {
                return err;
            }
            return ESP_ERR_NOT_FOUND;
        }

        err = rest_api_send_error_with_retryable(req,
                                                 503,
                                                 "MODIO_SAMPLE_UNAVAILABLE",
                                                 "MOD-IO input sample is not available yet",
                                                 true,
                                                 true);
        if (err != ESP_OK) {
            return err;
        }
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        return rest_api_send_error(req, 500, "INPUT_UNAVAILABLE", "Failed to read input snapshot", true);
    }

    out_status->modio_present = out_snapshot->modio_present;
    if (!out_snapshot->modio_present) {
        out_status->modio_sync = REST_API_MODIO_SYNC_ABSENT;
        err = rest_api_send_error(req, 503, "MODIO_NOT_PRESENT", "MOD-IO is not present", true);
        if (err != ESP_OK) {
            return err;
        }
        return ESP_ERR_NOT_FOUND;
    }

    if (!out_snapshot->sample_valid) {
        err = rest_api_send_error_with_retryable(req,
                                                 503,
                                                 "MODIO_SAMPLE_UNAVAILABLE",
                                                 "MOD-IO input sample is not available yet",
                                                 true,
                                                 true);
        if (err != ESP_OK) {
            return err;
        }
        return ESP_ERR_NOT_FOUND;
    }

    err = device_config_get_poll_interval_ms(out_poll_interval_ms);
    if (err != ESP_OK) {
        return rest_api_send_error(req,
                                   500,
                                   "INPUT_UNAVAILABLE",
                                   "Failed to read input polling interval",
                                   true);
    }

    *out_staleness_ms = rest_api_timestamp_ms();
    if (*out_staleness_ms >= out_snapshot->sample_ts_ms) {
        *out_staleness_ms -= out_snapshot->sample_ts_ms;
    } else {
        *out_staleness_ms = 0U;
    }

    return ESP_OK;
}

static const char *rest_api_config_response_key_name(device_config_key_t key)
{
    if (key == DEVICE_CONFIG_KEY_API_TOKEN) {
        return "api_token_set";
    }
    if (key == DEVICE_CONFIG_KEY_WIFI_SSID) {
        return "wifi_ssid_set";
    }
    if (key == DEVICE_CONFIG_KEY_WIFI_PASSPHRASE) {
        return "wifi_passphrase_set";
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
    cJSON *wifi = NULL;

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
    wifi = cJSON_CreateObject();
    if (wifi == NULL) {
        cJSON_Delete(config);
        return NULL;
    }
    cJSON_AddBoolToObject(wifi, "ssid_set", snapshot->wifi_ssid_set);
    cJSON_AddBoolToObject(wifi, "passphrase_set", snapshot->wifi_passphrase_set);
    cJSON_AddStringToObject(wifi,
                            "network_policy",
                            device_config_network_policy_to_string(snapshot->network_policy));
    cJSON_AddItemToObject(config, "wifi", wifi);
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
    case DEVICE_CONFIG_KEY_WIFI_SSID:
        return cJSON_CreateBool(snapshot->wifi_ssid_set);
    case DEVICE_CONFIG_KEY_WIFI_PASSPHRASE:
        return cJSON_CreateBool(snapshot->wifi_passphrase_set);
    case DEVICE_CONFIG_KEY_NETWORK_POLICY:
        return cJSON_CreateString(device_config_network_policy_to_string(snapshot->network_policy));
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

static esp_err_t rest_api_parse_wifi_config_update_request(const cJSON *root,
                                                           rest_api_wifi_config_update_request_t *out_request,
                                                           const char **out_error_code,
                                                           const char **out_error_message)
{
    bool has_updates = false;
    const cJSON *item = NULL;

    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_INVALID_ARG, TAG, "WiFi config JSON root is required");
    ESP_RETURN_ON_FALSE(out_request != NULL, ESP_ERR_INVALID_ARG, TAG, "WiFi config request output is required");

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

        if (strcmp(item->string, "ssid") == 0) {
            has_updates = true;
            if (cJSON_IsNull(item)) {
                if (rest_api_wifi_config_update_set_ssid(out_request,
                                                         NULL,
                                                         true,
                                                         out_error_code,
                                                         out_error_message) != ESP_OK) {
                    return ESP_ERR_INVALID_ARG;
                }
                continue;
            }
            if (!cJSON_IsString(item) || (item->valuestring == NULL)) {
                if (rest_api_wifi_config_update_set_ssid(out_request,
                                                         NULL,
                                                         false,
                                                         out_error_code,
                                                         out_error_message) != ESP_OK) {
                    return ESP_ERR_INVALID_ARG;
                }
                return ESP_ERR_INVALID_ARG;
            }
            if (rest_api_wifi_config_update_set_ssid(out_request,
                                                     item->valuestring,
                                                     false,
                                                     out_error_code,
                                                     out_error_message) != ESP_OK) {
                return ESP_ERR_INVALID_ARG;
            }
            continue;
        }

        if (strcmp(item->string, "passphrase") == 0) {
            if (!cJSON_IsString(item) || (item->valuestring == NULL)) {
                if (rest_api_wifi_config_update_set_passphrase(out_request,
                                                               NULL,
                                                               out_error_code,
                                                               out_error_message) != ESP_OK) {
                    return ESP_ERR_INVALID_ARG;
                }
                return ESP_ERR_INVALID_ARG;
            }
            if (rest_api_wifi_config_update_set_passphrase(out_request,
                                                           item->valuestring,
                                                           out_error_code,
                                                           out_error_message) != ESP_OK) {
                return ESP_ERR_INVALID_ARG;
            }
            continue;
        }

        if (strcmp(item->string, "network_policy") == 0) {
            has_updates = true;
            if (!cJSON_IsString(item) || (item->valuestring == NULL)) {
                if (rest_api_wifi_config_update_set_network_policy(out_request,
                                                                   NULL,
                                                                   out_error_code,
                                                                   out_error_message) != ESP_OK) {
                    return ESP_ERR_INVALID_ARG;
                }
                return ESP_ERR_INVALID_ARG;
            }
            if (rest_api_wifi_config_update_set_network_policy(out_request,
                                                               item->valuestring,
                                                               out_error_code,
                                                               out_error_message) != ESP_OK) {
                return ESP_ERR_INVALID_ARG;
            }
            continue;
        }

        rest_api_set_config_parse_error(out_error_code,
                                        out_error_message,
                                        "INVALID_CONFIG_KEY",
                                        "Request body contains an unsupported config key");
        return ESP_ERR_INVALID_ARG;
    }

    return rest_api_wifi_config_update_validate(out_request,
                                                has_updates,
                                                out_error_code,
                                                out_error_message);
}

static esp_err_t rest_api_config_handler(httpd_req_t *req)
{
    rest_api_status_view_t status;
    device_config_snapshot_t snapshot;
    cJSON *root = NULL;
    cJSON *config = NULL;
    esp_err_t err;

    if (!rest_api_require_authenticated_status(req, &status, &err)) {
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

static esp_err_t rest_api_send_ota_success_response(httpd_req_t *req,
                                                    const rest_api_status_view_t *status,
                                                    const ota_target_info_t *target,
                                                    size_t bytes_received)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *ota = cJSON_CreateObject();

    if ((root == NULL) || (ota == NULL) || (target == NULL)) {
        cJSON_Delete(root);
        cJSON_Delete(ota);
        return rest_api_send_error(req,
                                   500,
                                   "INTERNAL_ERROR",
                                   "Failed to allocate JSON response",
                                   true);
    }

    cJSON_AddStringToObject(ota, "partition", target->partition_label);
    cJSON_AddNumberToObject(ota, "partition_size", (double)target->partition_size);
    cJSON_AddNumberToObject(ota, "bytes_received", (double)bytes_received);
    cJSON_AddNumberToObject(ota, "reboot_delay_ms", (double)OTA_REBOOT_DELAY_MS);
    cJSON_AddItemToObject(root, "ota", ota);
    return rest_api_send_json_response(req, 200, root, status, true);
}

static esp_err_t rest_api_ota_handler(httpd_req_t *req)
{
    rest_api_status_view_t status;
    ota_target_info_t target = {0};
    uint8_t chunk[REST_API_OTA_UPLOAD_CHUNK_LEN];
    size_t remaining;
    size_t bytes_received = 0U;
    esp_err_t err;
    esp_err_t response_err;

    if (!rest_api_require_authenticated_status(req, &status, &err)) {
        return err;
    }

    if (req->content_len <= 0) {
        return rest_api_send_error(req,
                                   400,
                                   "INVALID_BODY",
                                   "Request body must contain a firmware image",
                                   true);
    }

    err = ota_begin_update((size_t)req->content_len, &target);
    if (err == ESP_ERR_INVALID_SIZE) {
        return rest_api_send_error(req,
                                   413,
                                   "OTA_IMAGE_TOO_LARGE",
                                   "Firmware image does not fit the OTA slot",
                                   true);
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return rest_api_send_error_with_retryable(req,
                                                  409,
                                                  "OTA_BUSY",
                                                  "Device is not ready for another OTA update",
                                                  true,
                                                  true);
    }
    if (err == ESP_ERR_OTA_ROLLBACK_INVALID_STATE) {
        return rest_api_send_error_with_retryable(req,
                                                  409,
                                                  "OTA_PENDING_VERIFY",
                                                  "Running firmware must be confirmed before another OTA update",
                                                  true,
                                                  true);
    }
    if (err == ESP_ERR_NOT_FOUND) {
        return rest_api_send_error_with_retryable(req,
                                                  503,
                                                  "OTA_UNAVAILABLE",
                                                  "OTA partition is not available",
                                                  true,
                                                  true);
    }
    if (err != ESP_OK) {
        return rest_api_send_error_with_retryable(req,
                                                  503,
                                                  "OTA_UNAVAILABLE",
                                                  "Failed to start OTA update",
                                                  true,
                                                  true);
    }

    remaining = (size_t)req->content_len;
    while (remaining > 0U) {
        size_t chunk_len = remaining;

        if (chunk_len > sizeof(chunk)) {
            chunk_len = sizeof(chunk);
        }

        err = rest_api_request_recv_exact(req, (char *)chunk, chunk_len, NULL);
        if (err == ESP_ERR_TIMEOUT) {
            ota_abort_update();
            return rest_api_send_error_with_retryable(req,
                                                      408,
                                                      "REQUEST_TIMEOUT",
                                                      "Timed out while receiving the firmware image",
                                                      true,
                                                      true);
        }
        if (err != ESP_OK) {
            ota_abort_update();
            return rest_api_send_error_with_retryable(req,
                                                      400,
                                                      "INVALID_BODY",
                                                      "Failed to receive the firmware image",
                                                      true,
                                                      true);
        }

        err = ota_write_chunk(chunk, chunk_len);
        if (err != ESP_OK) {
            ota_abort_update();
            if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
                return rest_api_send_error(req,
                                           400,
                                           "INVALID_FIRMWARE_IMAGE",
                                           "Firmware image is not valid",
                                           true);
            }
            return rest_api_send_error_with_retryable(req,
                                                      503,
                                                      "OTA_WRITE_FAILED",
                                                      "Failed to write the firmware image",
                                                      true,
                                                      true);
        }

        remaining -= chunk_len;
        bytes_received += chunk_len;
    }

    err = ota_finalize_update();
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            return rest_api_send_error(req,
                                       400,
                                       "INVALID_FIRMWARE_IMAGE",
                                       "Firmware image is not valid",
                                       true);
        }
        return rest_api_send_error_with_retryable(req,
                                                  503,
                                                  "OTA_FINALIZE_FAILED",
                                                  "Failed to finalize the firmware image",
                                                  true,
                                                  false);
    }

    response_err = rest_api_send_ota_success_response(req, &status, &target, bytes_received);
    err = ota_schedule_reboot();
    if ((response_err == ESP_OK) && (err != ESP_OK)) {
        return err;
    }

    return response_err;
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

    if (!rest_api_require_authenticated_status(req, &status, &err)) {
        return err;
    }

    err = rest_api_read_request_body(req, request_body, sizeof(request_body), &request_len);
    if (err != ESP_OK) {
        return rest_api_send_request_body_read_error(req, err);
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

static esp_err_t rest_api_wifi_config_update_handler(httpd_req_t *req)
{
    rest_api_status_view_t status;
    device_config_snapshot_t before_snapshot;
    device_config_snapshot_t after_snapshot;
    rest_api_wifi_config_update_request_t update_request;
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

    if (!rest_api_require_authenticated_status(req, &status, &err)) {
        return err;
    }

    err = rest_api_read_request_body(req, request_body, sizeof(request_body), &request_len);
    if (err != ESP_OK) {
        return rest_api_send_request_body_read_error(req, err);
    }

    request_root = cJSON_ParseWithLength(request_body, request_len);
    if (request_root == NULL) {
        return rest_api_send_error(req, 400, "INVALID_JSON", "Request body must be valid JSON", true);
    }

    err = rest_api_parse_wifi_config_update_request(request_root,
                                                    &update_request,
                                                    &error_code,
                                                    &error_message);
    if (err != ESP_OK) {
        cJSON_Delete(request_root);
        return rest_api_send_error(req,
                                   400,
                                   (error_code != NULL) ? error_code : "INVALID_CONFIG",
                                   (error_message != NULL) ? error_message : "Invalid WiFi config request",
                                   true);
    }

    err = device_config_get_snapshot(&before_snapshot);
    if (err != ESP_OK) {
        cJSON_Delete(request_root);
        return rest_api_send_error(req, 500, "CONFIG_UNAVAILABLE", "Failed to read device config", true);
    }

    if (update_request.credentials_present) {
        requested[DEVICE_CONFIG_KEY_WIFI_SSID] = true;
        requested[DEVICE_CONFIG_KEY_WIFI_PASSPHRASE] = true;
        err = device_config_set_wifi_sta_credentials(update_request.credentials_clear ? NULL : update_request.ssid,
                                                     update_request.credentials_clear ? NULL : update_request.passphrase,
                                                     &apply_results[DEVICE_CONFIG_KEY_WIFI_SSID]);
        apply_results[DEVICE_CONFIG_KEY_WIFI_PASSPHRASE] = apply_results[DEVICE_CONFIG_KEY_WIFI_SSID];
        if (err == ESP_ERR_INVALID_ARG) {
            cJSON_Delete(request_root);
            return rest_api_send_error(req,
                                       400,
                                       "INVALID_CONFIG_VALUE",
                                       "WiFi credentials are invalid",
                                       true);
        }
        if (err != ESP_OK) {
            cJSON_Delete(request_root);
            return rest_api_send_error(req,
                                       500,
                                       "CONFIG_UPDATE_FAILED",
                                       "Failed to update WiFi credentials",
                                       true);
        }
    }

    if (update_request.network_policy_present) {
        requested[DEVICE_CONFIG_KEY_NETWORK_POLICY] = true;
        err = device_config_set_network_policy(update_request.network_policy,
                                               &apply_results[DEVICE_CONFIG_KEY_NETWORK_POLICY]);
        if (err == ESP_ERR_INVALID_ARG) {
            cJSON_Delete(request_root);
            return rest_api_send_error(req,
                                       400,
                                       "INVALID_CONFIG_VALUE",
                                       "network_policy is invalid",
                                       true);
        }
        if (err != ESP_OK) {
            cJSON_Delete(request_root);
            return rest_api_send_error(req,
                                       500,
                                       "CONFIG_UPDATE_FAILED",
                                       "Failed to update network_policy",
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

    if (!rest_api_require_authenticated_status(req, &status, &err)) {
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

    if (!rest_api_require_authenticated_status(req, &status, &err)) {
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

    if (!rest_api_require_authenticated_status(req, &status, &err)) {
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

    if (!rest_api_require_authenticated_status(req, &status, &err)) {
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

    if (!rest_api_require_authenticated_status(req, &status, &err)) {
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
    esp_err_t err;

    if (!rest_api_require_authenticated_status(req, &status, &err)) {
        return err;
    }

    err = rest_api_parse_modio_relay_id(req, NULL, &relay_id);
    if (err != ESP_OK) {
        return err;
    }

    err = rest_api_parse_boolean_state_request(req, &requested_state);
    if (err != ESP_OK) {
        return err;
    }

    err = rest_api_sync_status_with_modio_driver(&status, NULL);
    if (err == ESP_ERR_NOT_FOUND) {
        return rest_api_send_error_with_status(req,
                                               503,
                                               "MODIO_NOT_PRESENT",
                                               "MOD-IO is not present",
                                               &status,
                                               true);
    }
    if (err != ESP_OK) {
        return rest_api_send_error_with_status(req,
                                               500,
                                               "MODIO_UNAVAILABLE",
                                               "Failed to read MOD-IO relay state",
                                               &status,
                                               true);
    }

    err = mod_io_set_relay(relay_id, requested_state);
    if (err == ESP_ERR_NOT_FOUND) {
        return rest_api_send_error_with_status(req,
                                               503,
                                               "MODIO_NOT_PRESENT",
                                               "MOD-IO is not present",
                                               &status,
                                               true);
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return rest_api_send_error_with_status(req,
                                               409,
                                               "MODIO_STATE_UNKNOWN",
                                               "MOD-IO relay state is unknown until a full-mask write succeeds",
                                               &status,
                                               true);
    }
    if (err != ESP_OK) {
        return rest_api_send_error_with_status(req,
                                               500,
                                               "MODIO_SET_FAILED",
                                               "Failed to set MOD-IO relay state",
                                               &status,
                                               true);
    }

    err = rest_api_sync_status_with_modio_driver(&status, &relay_mask);
    if (err == ESP_ERR_NOT_FOUND) {
        return rest_api_send_error_with_status(req,
                                               503,
                                               "MODIO_NOT_PRESENT",
                                               "MOD-IO is not present",
                                               &status,
                                               true);
    }
    if (err != ESP_OK) {
        return rest_api_send_error_with_status(req,
                                               500,
                                               "MODIO_UNAVAILABLE",
                                               "Failed to read MOD-IO relay state",
                                               &status,
                                               true);
    }

    return rest_api_send_modio_relay_response(req,
                                              &status,
                                              relay_id,
                                              (relay_mask & (uint8_t)(1U << (relay_id - 1U))) != 0U);
}

static esp_err_t rest_api_modio_relay_toggle_handler(httpd_req_t *req)
{
    rest_api_status_view_t status;
    uint8_t relay_id = 0;
    uint8_t relay_mask = 0;
    esp_err_t err;

    if (!rest_api_require_authenticated_status(req, &status, &err)) {
        return err;
    }

    err = rest_api_parse_modio_relay_id(req, REST_API_MODIO_RELAY_TOGGLE_SUFFIX, &relay_id);
    if (err != ESP_OK) {
        return err;
    }

    err = rest_api_sync_status_with_modio_driver(&status, NULL);
    if (err == ESP_ERR_NOT_FOUND) {
        return rest_api_send_error_with_status(req,
                                               503,
                                               "MODIO_NOT_PRESENT",
                                               "MOD-IO is not present",
                                               &status,
                                               true);
    }
    if (err != ESP_OK) {
        return rest_api_send_error_with_status(req,
                                               500,
                                               "MODIO_UNAVAILABLE",
                                               "Failed to read MOD-IO relay state",
                                               &status,
                                               true);
    }

    err = mod_io_toggle_relay(relay_id, NULL);
    if (err == ESP_ERR_NOT_FOUND) {
        return rest_api_send_error_with_status(req,
                                               503,
                                               "MODIO_NOT_PRESENT",
                                               "MOD-IO is not present",
                                               &status,
                                               true);
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return rest_api_send_error_with_status(req,
                                               409,
                                               "MODIO_STATE_UNKNOWN",
                                               "MOD-IO relay state is unknown until a full-mask write succeeds",
                                               &status,
                                               true);
    }
    if (err != ESP_OK) {
        return rest_api_send_error_with_status(req,
                                               500,
                                               "MODIO_TOGGLE_FAILED",
                                               "Failed to toggle MOD-IO relay state",
                                               &status,
                                               true);
    }

    err = rest_api_sync_status_with_modio_driver(&status, &relay_mask);
    if (err == ESP_ERR_NOT_FOUND) {
        return rest_api_send_error_with_status(req,
                                               503,
                                               "MODIO_NOT_PRESENT",
                                               "MOD-IO is not present",
                                               &status,
                                               true);
    }
    if (err != ESP_OK) {
        return rest_api_send_error_with_status(req,
                                               500,
                                               "MODIO_UNAVAILABLE",
                                               "Failed to read MOD-IO relay state",
                                               &status,
                                               true);
    }

    return rest_api_send_modio_relay_response(req,
                                              &status,
                                              relay_id,
                                              (relay_mask & (uint8_t)(1U << (relay_id - 1U))) != 0U);
}

static esp_err_t rest_api_modio_relays_set_handler(httpd_req_t *req)
{
    rest_api_status_view_t status;
    cJSON *root = NULL;
    cJSON *relays = NULL;
    uint8_t relay_mask = 0;
    esp_err_t err;

    if (!rest_api_require_authenticated_status(req, &status, &err)) {
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

static esp_err_t rest_api_digital_inputs_handler(httpd_req_t *req)
{
    rest_api_status_view_t status;
    input_monitor_snapshot_t snapshot;
    uint32_t poll_interval_ms = 0;
    uint64_t staleness_ms = 0U;
    cJSON *root = NULL;
    cJSON *inputs = NULL;
    esp_err_t err;

    err = rest_api_prepare_input_snapshot(req,
                                          &status,
                                          &snapshot,
                                          &poll_interval_ms,
                                          &staleness_ms);
    if (err != ESP_OK) {
        return err;
    }

    root = cJSON_CreateObject();
    inputs = cJSON_CreateArray();
    if ((root == NULL) || (inputs == NULL)) {
        cJSON_Delete(root);
        cJSON_Delete(inputs);
        return rest_api_send_error(req,
                                   500,
                                   "INTERNAL_ERROR",
                                   "Failed to allocate JSON response",
                                   true);
    }

    rest_api_add_sample_metadata(root, snapshot.sample_ts_ms, staleness_ms, poll_interval_ms);
    err = rest_api_append_digital_input_objects(inputs, snapshot.digital_mask);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        cJSON_Delete(inputs);
        return rest_api_send_error(req,
                                   500,
                                   "INTERNAL_ERROR",
                                   "Failed to allocate JSON response",
                                   true);
    }

    cJSON_AddItemToObject(root, "inputs", inputs);
    return rest_api_send_json_response(req, 200, root, &status, true);
}

static esp_err_t rest_api_digital_input_handler(httpd_req_t *req)
{
    rest_api_status_view_t status;
    input_monitor_snapshot_t snapshot;
    uint32_t poll_interval_ms = 0;
    uint64_t staleness_ms = 0U;
    uint8_t input_id = 0U;
    cJSON *root = NULL;
    cJSON *input = NULL;
    esp_err_t err;

    err = rest_api_prepare_input_snapshot(req,
                                          &status,
                                          &snapshot,
                                          &poll_interval_ms,
                                          &staleness_ms);
    if (err != ESP_OK) {
        return err;
    }

    err = rest_api_parse_input_id(req,
                                  REST_API_DIGITAL_INPUT_ID_PREFIX,
                                  MOD_IO_DIGITAL_INPUT_COUNT,
                                  &input_id);
    if (err != ESP_OK) {
        return err;
    }

    root = cJSON_CreateObject();
    input = rest_api_create_digital_input_object(
                input_id,
                (snapshot.digital_mask & (uint8_t)(1U << (input_id - 1U))) != 0U);
    if ((root == NULL) || (input == NULL)) {
        cJSON_Delete(root);
        cJSON_Delete(input);
        return rest_api_send_error(req,
                                   500,
                                   "INTERNAL_ERROR",
                                   "Failed to allocate JSON response",
                                   true);
    }

    rest_api_add_sample_metadata(root, snapshot.sample_ts_ms, staleness_ms, poll_interval_ms);
    cJSON_AddItemToObject(root, "input", input);
    return rest_api_send_json_response(req, 200, root, &status, true);
}

static esp_err_t rest_api_analog_inputs_handler(httpd_req_t *req)
{
    rest_api_status_view_t status;
    input_monitor_snapshot_t snapshot;
    uint32_t poll_interval_ms = 0;
    uint64_t staleness_ms = 0U;
    cJSON *root = NULL;
    cJSON *inputs = NULL;
    esp_err_t err;

    err = rest_api_prepare_input_snapshot(req,
                                          &status,
                                          &snapshot,
                                          &poll_interval_ms,
                                          &staleness_ms);
    if (err != ESP_OK) {
        return err;
    }

    root = cJSON_CreateObject();
    inputs = cJSON_CreateArray();
    if ((root == NULL) || (inputs == NULL)) {
        cJSON_Delete(root);
        cJSON_Delete(inputs);
        return rest_api_send_error(req,
                                   500,
                                   "INTERNAL_ERROR",
                                   "Failed to allocate JSON response",
                                   true);
    }

    rest_api_add_sample_metadata(root, snapshot.sample_ts_ms, staleness_ms, poll_interval_ms);
    err = rest_api_append_analog_input_objects(inputs, snapshot.analog_values);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        cJSON_Delete(inputs);
        return rest_api_send_error(req,
                                   500,
                                   "INTERNAL_ERROR",
                                   "Failed to allocate JSON response",
                                   true);
    }

    cJSON_AddItemToObject(root, "inputs", inputs);
    return rest_api_send_json_response(req, 200, root, &status, true);
}

static esp_err_t rest_api_analog_input_handler(httpd_req_t *req)
{
    rest_api_status_view_t status;
    input_monitor_snapshot_t snapshot;
    uint32_t poll_interval_ms = 0;
    uint64_t staleness_ms = 0U;
    uint8_t input_id = 0U;
    cJSON *root = NULL;
    cJSON *input = NULL;
    esp_err_t err;

    err = rest_api_prepare_input_snapshot(req,
                                          &status,
                                          &snapshot,
                                          &poll_interval_ms,
                                          &staleness_ms);
    if (err != ESP_OK) {
        return err;
    }

    err = rest_api_parse_input_id(req,
                                  REST_API_ANALOG_INPUT_ID_PREFIX,
                                  MOD_IO_ANALOG_INPUT_COUNT,
                                  &input_id);
    if (err != ESP_OK) {
        return err;
    }

    root = cJSON_CreateObject();
    input = rest_api_create_analog_input_object(input_id, snapshot.analog_values[input_id - 1U]);
    if ((root == NULL) || (input == NULL)) {
        cJSON_Delete(root);
        cJSON_Delete(input);
        return rest_api_send_error(req,
                                   500,
                                   "INTERNAL_ERROR",
                                   "Failed to allocate JSON response",
                                   true);
    }

    rest_api_add_sample_metadata(root, snapshot.sample_ts_ms, staleness_ms, poll_interval_ms);
    cJSON_AddItemToObject(root, "input", input);
    return rest_api_send_json_response(req, 200, root, &status, true);
}

static esp_err_t rest_api_status_handler(httpd_req_t *req)
{
    rest_api_status_view_t status;
    cJSON *root = NULL;
    cJSON *network = NULL;
    cJSON *modio = NULL;
    esp_err_t err;

    if (!rest_api_require_authenticated_status(req, &status, &err)) {
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
    cJSON_AddStringToObject(network, "transport", rest_api_network_transport_to_string(status.network.transport));
    cJSON_AddStringToObject(network, "ip", status.network.ip);
    cJSON_AddStringToObject(network, "netmask", status.network.netmask);
    cJSON_AddStringToObject(network, "gateway", status.network.gateway);
    cJSON_AddItemToObject(root, "network", network);

    cJSON_AddBoolToObject(modio, "present", status.modio_present);
    cJSON_AddStringToObject(modio, "sync", rest_api_modio_sync_to_string(status.modio_sync));
    cJSON_AddItemToObject(root, "modio", modio);
    return rest_api_send_json_response(req, 200, root, &status, true);
}

static esp_err_t rest_api_send_error_internal(httpd_req_t *req,
                                              int http_status,
                                              const char *code,
                                              const char *message,
                                              const rest_api_status_view_t *status_override,
                                              bool authenticated,
                                              bool include_retryable,
                                              bool retryable)
{
    rest_api_status_view_t header_status;
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
    if (include_retryable) {
        cJSON_AddBoolToObject(error_obj, "retryable", retryable);
    }
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
    if ((http_status == 401) || (http_status == 403)) {
        httpd_resp_set_hdr(req, "Connection", "close");
    }

    if (authenticated && (http_status != 401) && (http_status != 403)) {
        if (status_override != NULL) {
            rest_api_try_attach_device_context_headers(req, status_override, true);
        } else {
            err = rest_api_build_status_view(&header_status);
            if (err == ESP_OK) {
                rest_api_try_attach_device_context_headers(req, &header_status, true);
            }
        }
    }

    err = httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    cJSON_free(response);
    return err;
}

static esp_err_t rest_api_send_error_with_retryable(httpd_req_t *req,
                                                    int http_status,
                                                    const char *code,
                                                    const char *message,
                                                    bool authenticated,
                                                    bool retryable)
{
    return rest_api_send_error_internal(req,
                                        http_status,
                                        code,
                                        message,
                                        NULL,
                                        authenticated,
                                        true,
                                        retryable);
}

static esp_err_t rest_api_send_request_body_read_error(httpd_req_t *req, esp_err_t err)
{
    if (err == ESP_ERR_INVALID_SIZE) {
        return rest_api_send_error(req,
                                   400,
                                   "INVALID_BODY",
                                   "Request body is missing or too large",
                                   true);
    }

    if (err == ESP_ERR_TIMEOUT) {
        return rest_api_send_error_with_retryable(req,
                                                  408,
                                                  "REQUEST_TIMEOUT",
                                                  "Timed out while receiving request body",
                                                  true,
                                                  true);
    }

    return rest_api_send_error(req,
                               400,
                               "INVALID_BODY",
                               "Failed to read request body",
                               true);
}

esp_err_t rest_api_send_error(httpd_req_t *req,
                              int http_status,
                              const char *code,
                              const char *message,
                              bool authenticated)
{
    return rest_api_send_error_internal(req,
                                        http_status,
                                        code,
                                        message,
                                        NULL,
                                        authenticated,
                                        false,
                                        false);
}

static esp_err_t rest_api_send_error_with_status(httpd_req_t *req,
                                                 int http_status,
                                                 const char *code,
                                                 const char *message,
                                                 const rest_api_status_view_t *status,
                                                 bool authenticated)
{
    return rest_api_send_error_internal(req,
                                        http_status,
                                        code,
                                        message,
                                        status,
                                        authenticated,
                                        false,
                                        false);
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
        .uri = "/api/v1/relays/onboard/*",
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
    httpd_uri_t modio_relay_toggle_uri = {
        .uri = "/api/v1/relays/modio/*",
        .method = HTTP_POST,
        .handler = rest_api_modio_relay_toggle_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t modio_relays_set_uri = {
        .uri = REST_API_MODIO_RELAYS_URI,
        .method = HTTP_PUT,
        .handler = rest_api_modio_relays_set_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t digital_inputs_uri = {
        .uri = REST_API_DIGITAL_INPUTS_URI,
        .method = HTTP_GET,
        .handler = rest_api_digital_inputs_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t digital_input_uri = {
        .uri = "/api/v1/inputs/digital/*",
        .method = HTTP_GET,
        .handler = rest_api_digital_input_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t analog_inputs_uri = {
        .uri = REST_API_ANALOG_INPUTS_URI,
        .method = HTTP_GET,
        .handler = rest_api_analog_inputs_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t analog_input_uri = {
        .uri = "/api/v1/inputs/analog/*",
        .method = HTTP_GET,
        .handler = rest_api_analog_input_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t events_uri = {
        .uri = REST_API_EVENTS_URI,
        .method = HTTP_GET,
        .handler = rest_api_events_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t ota_uri = {
        .uri = REST_API_OTA_URI,
        .method = HTTP_POST,
        .handler = rest_api_ota_handler,
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
    httpd_uri_t config_wifi_update_uri = {
        .uri = REST_API_CONFIG_WIFI_URI,
        .method = HTTP_PUT,
        .handler = rest_api_wifi_config_update_handler,
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
    server_config.max_uri_handlers = REST_API_URI_HANDLER_COUNT;
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

    err = httpd_register_uri_handler(s_server, &onboard_relay_toggle_uri);
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

    err = httpd_register_uri_handler(s_server, &modio_relay_toggle_uri);
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

    err = httpd_register_uri_handler(s_server, &digital_inputs_uri);
    if (err != ESP_OK) {
        httpd_stop(s_server);
        s_server = NULL;
        memset(&s_config, 0, sizeof(s_config));
        return err;
    }

    err = httpd_register_uri_handler(s_server, &digital_input_uri);
    if (err != ESP_OK) {
        httpd_stop(s_server);
        s_server = NULL;
        memset(&s_config, 0, sizeof(s_config));
        return err;
    }

    err = httpd_register_uri_handler(s_server, &analog_inputs_uri);
    if (err != ESP_OK) {
        httpd_stop(s_server);
        s_server = NULL;
        memset(&s_config, 0, sizeof(s_config));
        return err;
    }

    err = httpd_register_uri_handler(s_server, &analog_input_uri);
    if (err != ESP_OK) {
        httpd_stop(s_server);
        s_server = NULL;
        memset(&s_config, 0, sizeof(s_config));
        return err;
    }

    err = httpd_register_uri_handler(s_server, &events_uri);
    if (err != ESP_OK) {
        httpd_stop(s_server);
        s_server = NULL;
        memset(&s_config, 0, sizeof(s_config));
        return err;
    }

    err = httpd_register_uri_handler(s_server, &ota_uri);
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

    err = httpd_register_uri_handler(s_server, &config_wifi_update_uri);
    if (err != ESP_OK) {
        httpd_stop(s_server);
        s_server = NULL;
        memset(&s_config, 0, sizeof(s_config));
        return err;
    }

    err = rest_api_sse_start();
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

    rest_api_sse_stop();
    ESP_RETURN_ON_ERROR(httpd_stop(s_server), TAG, "Failed to stop REST API server");
    s_server = NULL;
    memset(&s_config, 0, sizeof(s_config));

    return ESP_OK;
}

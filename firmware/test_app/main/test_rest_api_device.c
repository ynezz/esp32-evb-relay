#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <sys/time.h>

#include "auth.h"
#include "board.h"
#include "device_config.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "relay.h"
#include "rest_api.h"
#include "unity.h"

static void ensure_tcpip_ready(void)
{
    static bool tcpip_ready;
    esp_err_t err;

    if (tcpip_ready) {
        return;
    }

    err = esp_netif_init();
    TEST_ASSERT_TRUE((err == ESP_OK) || (err == ESP_ERR_INVALID_STATE));

    for (int i = 0; i < CONFIG_LWIP_MAX_SOCKETS; ++i) {
        int family = ((i % 3) == 0) ? PF_INET6 : PF_INET;
        int type = ((i % 2) == 0) ? SOCK_DGRAM : SOCK_STREAM;
        int sock = socket(family, type, IPPROTO_IP);

        TEST_ASSERT_GREATER_OR_EQUAL_INT32(0, sock);
        close(sock);
    }

    vTaskDelay(pdMS_TO_TICKS(25));
    tcpip_ready = true;
}

static rest_api_auth_result_t allow_auth_handler(httpd_req_t *req, void *ctx)
{
    (void)req;
    (void)ctx;
    return REST_API_AUTH_RESULT_ALLOW;
}

static rest_api_auth_result_t unauthorized_auth_handler(httpd_req_t *req, void *ctx)
{
    (void)req;
    (void)ctx;
    return REST_API_AUTH_RESULT_UNAUTHORIZED;
}

static rest_api_auth_result_t forbidden_auth_handler(httpd_req_t *req, void *ctx)
{
    (void)req;
    (void)ctx;
    return REST_API_AUTH_RESULT_FORBIDDEN;
}

static esp_err_t status_provider(rest_api_status_view_t *status, void *ctx)
{
    (void)ctx;

    TEST_ASSERT_NOT_NULL(status);
    status->network.connected = true;
    strncpy(status->network.hostname, "loopback-relay", sizeof(status->network.hostname) - 1U);
    strncpy(status->network.ip, "127.0.0.1", sizeof(status->network.ip) - 1U);
    strncpy(status->network.netmask, "255.0.0.0", sizeof(status->network.netmask) - 1U);
    strncpy(status->network.gateway, "127.0.0.1", sizeof(status->network.gateway) - 1U);
    status->modio_present = true;
    status->modio_sync = REST_API_MODIO_SYNC_SYNCHRONIZED;
    return ESP_OK;
}

typedef struct {
    char api_token[DEVICE_CONFIG_API_TOKEN_MAX_LEN + 1];
    bool api_token_set;
} auth_token_fixture_t;

static void capture_auth_token_fixture(auth_token_fixture_t *fixture)
{
    TEST_ASSERT_NOT_NULL(fixture);
    TEST_ASSERT_EQUAL(ESP_OK,
                      device_config_get_api_token(fixture->api_token,
                                                  sizeof(fixture->api_token),
                                                  &fixture->api_token_set));
}

static void restore_auth_token_fixture(const auth_token_fixture_t *fixture)
{
    TEST_ASSERT_NOT_NULL(fixture);
    if (fixture->api_token_set) {
        TEST_ASSERT_EQUAL(ESP_OK, device_config_set_api_token(fixture->api_token, NULL));
    } else {
        TEST_ASSERT_EQUAL(ESP_OK, device_config_set_api_token(NULL, NULL));
    }
}

static void perform_http_request(uint16_t port,
                                 const char *method,
                                 const char *path,
                                 const char *authorization_header,
                                 const char *body,
                                 char *response,
                                 size_t response_size)
{
    char request[768];
    struct sockaddr_in dest_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    struct timeval timeout = {
        .tv_sec = 2,
        .tv_usec = 0,
    };
    int sock;
    ssize_t sent;
    ssize_t received;
    int written;
    size_t request_len = 0;

    TEST_ASSERT_NOT_NULL(method);
    TEST_ASSERT_NOT_NULL(path);
    TEST_ASSERT_NOT_NULL(response);
    TEST_ASSERT_GREATER_THAN_UINT32(0U, response_size);
    memset(response, 0, response_size);

    written = snprintf(request + request_len,
                       sizeof(request) - request_len,
                       "%s %s HTTP/1.1\r\n"
                       "Host: localhost\r\n",
                       method,
                       path);
    TEST_ASSERT_GREATER_THAN_INT32(0, written);
    request_len += (size_t)written;
    TEST_ASSERT_LESS_THAN_UINT32(sizeof(request), request_len);

    if (authorization_header != NULL) {
        written = snprintf(request + request_len,
                           sizeof(request) - request_len,
                           "%s\r\n",
                           authorization_header);
        TEST_ASSERT_GREATER_THAN_INT32(0, written);
        request_len += (size_t)written;
        TEST_ASSERT_LESS_THAN_UINT32(sizeof(request), request_len);
    }

    if (body != NULL) {
        written = snprintf(request + request_len,
                           sizeof(request) - request_len,
                           "Content-Type: application/json\r\n"
                           "Content-Length: %u\r\n",
                           (unsigned)strlen(body));
        TEST_ASSERT_GREATER_THAN_INT32(0, written);
        request_len += (size_t)written;
        TEST_ASSERT_LESS_THAN_UINT32(sizeof(request), request_len);
    }

    written = snprintf(request + request_len,
                       sizeof(request) - request_len,
                       "Connection: close\r\n"
                       "\r\n");
    TEST_ASSERT_GREATER_THAN_INT32(0, written);
    request_len += (size_t)written;
    TEST_ASSERT_LESS_THAN_UINT32(sizeof(request), request_len);

    if (body != NULL) {
        written = snprintf(request + request_len, sizeof(request) - request_len, "%s", body);
        TEST_ASSERT_GREATER_THAN_INT32(0, written);
        request_len += (size_t)written;
        TEST_ASSERT_LESS_THAN_UINT32(sizeof(request), request_len + 1U);
    }

    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    TEST_ASSERT_GREATER_OR_EQUAL_INT32(0, sock);
    TEST_ASSERT_GREATER_OR_EQUAL_INT32(0,
                                       setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    TEST_ASSERT_GREATER_OR_EQUAL_INT32(0,
                                       setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)));
    TEST_ASSERT_GREATER_OR_EQUAL_INT32(0,
                                       connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)));

    sent = send(sock, request, request_len, 0);
    TEST_ASSERT_EQUAL_INT((int)request_len, sent);

    received = recv(sock, response, response_size - 1U, 0);
    TEST_ASSERT_GREATER_THAN_INT32(0, received);
    response[received] = '\0';
    close(sock);
}

static void perform_status_request(uint16_t port,
                                   const char *authorization_header,
                                   char *response,
                                   size_t response_size)
{
    perform_http_request(port,
                         "GET",
                         "/api/v1/status",
                         authorization_header,
                         NULL,
                         response,
                         response_size);
}

TEST_CASE("rest_api device starts the HTTP server on the configured port",
          "[qa][rest_api][device]")
{
    static const uint16_t test_port = 18080U;
    static const rest_api_config_t config = {
        .port = test_port,
        .auth_handler = allow_auth_handler,
        .status_provider = status_provider,
    };
    char response[768];

    ensure_tcpip_ready();
    TEST_ASSERT_EQUAL(ESP_OK, rest_api_start(&config));
    TEST_ASSERT_NOT_NULL(rest_api_get_server());

    perform_status_request(test_port, NULL, response, sizeof(response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 200 OK"));
    TEST_ASSERT_NOT_NULL(strstr(response, "X-FW-Version: "));
    TEST_ASSERT_NOT_NULL(strstr(response, "X-ModIO-Present: true"));
    TEST_ASSERT_NOT_NULL(strstr(response, "X-ModIO-Sync: synchronized"));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"hostname\":\"loopback-relay\""));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"sync\":\"synchronized\""));
}

TEST_CASE("rest_api device exposes onboard relay endpoints", "[qa][rest_api][device]")
{
    static const uint16_t test_port = 18086U;
    static const rest_api_config_t config = {
        .port = test_port,
        .auth_handler = allow_auth_handler,
        .status_provider = status_provider,
    };
    char response[768];
    bool relay_state = false;

    ensure_tcpip_ready();
    TEST_ASSERT_EQUAL(ESP_OK, board_init());
    TEST_ASSERT_EQUAL(ESP_OK, relay_init());
    TEST_ASSERT_EQUAL(ESP_OK, rest_api_start(&config));

    perform_http_request(test_port, "GET", "/api/v1/relays/onboard", NULL, NULL, response, sizeof(response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 200 OK"));
    TEST_ASSERT_NOT_NULL(strstr(response, "X-FW-Version: "));
    TEST_ASSERT_NOT_NULL(strstr(response, "X-ModIO-Present: true"));
    TEST_ASSERT_NOT_NULL(strstr(response, "X-ModIO-Sync: synchronized"));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"group\":\"onboard\",\"id\":1,\"state\":false"));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"group\":\"onboard\",\"id\":2,\"state\":false"));

    perform_http_request(test_port,
                         "PUT",
                         "/api/v1/relays/onboard/1",
                         NULL,
                         "{\"state\":true}",
                         response,
                         sizeof(response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 200 OK"));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"relay\":{\"group\":\"onboard\",\"id\":1,\"state\":true}"));
    TEST_ASSERT_EQUAL(ESP_OK, relay_get(1U, &relay_state));
    TEST_ASSERT_TRUE(relay_state);

    perform_http_request(test_port,
                         "POST",
                         "/api/v1/relays/onboard/1/toggle",
                         NULL,
                         NULL,
                         response,
                         sizeof(response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 200 OK"));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"relay\":{\"group\":\"onboard\",\"id\":1,\"state\":false}"));
    TEST_ASSERT_EQUAL(ESP_OK, relay_get(1U, &relay_state));
    TEST_ASSERT_FALSE(relay_state);
}

TEST_CASE("rest_api device returns relay errors with device headers after auth",
          "[qa][rest_api][device]")
{
    static const uint16_t test_port = 18087U;
    static const rest_api_config_t config = {
        .port = test_port,
        .auth_handler = allow_auth_handler,
        .status_provider = status_provider,
    };
    char response[768];

    ensure_tcpip_ready();
    TEST_ASSERT_EQUAL(ESP_OK, board_init());
    TEST_ASSERT_EQUAL(ESP_OK, relay_init());
    TEST_ASSERT_EQUAL(ESP_OK, rest_api_start(&config));

    perform_http_request(test_port,
                         "PUT",
                         "/api/v1/relays/onboard/9",
                         NULL,
                         "{\"state\":true}",
                         response,
                         sizeof(response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 404 Not Found"));
    TEST_ASSERT_NOT_NULL(strstr(response, "X-FW-Version: "));
    TEST_ASSERT_NOT_NULL(strstr(response, "X-ModIO-Present: true"));
    TEST_ASSERT_NOT_NULL(strstr(response, "X-ModIO-Sync: synchronized"));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"code\":\"RELAY_NOT_FOUND\""));
    TEST_ASSERT_NULL(strstr(response, "HTTP/1.1 500 Internal Server Error"));
    TEST_ASSERT_NULL(strstr(response, "\"code\":\"RELAY_SET_FAILED\""));
}

TEST_CASE("rest_api device parses relay IDs from wildcard URIs",
          "[qa][rest_api][device]")
{
    uint32_t id = 0;

    TEST_ASSERT_TRUE(rest_api_parse_id_from_uri("/api/v1/relays/1", "/api/v1/relays/", &id));
    TEST_ASSERT_EQUAL_UINT32(1U, id);

    TEST_ASSERT_TRUE(rest_api_parse_id_from_uri("/api/v1/relays/4294967295",
                                                "/api/v1/relays/",
                                                &id));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, id);

    TEST_ASSERT_FALSE(rest_api_parse_id_from_uri("/api/v1/relays/", "/api/v1/relays/", &id));
    TEST_ASSERT_FALSE(rest_api_parse_id_from_uri("/api/v1/relays/0", "/api/v1/relays/", &id));
    TEST_ASSERT_FALSE(rest_api_parse_id_from_uri("/api/v1/relays/not-a-number",
                                                 "/api/v1/relays/",
                                                 &id));
    TEST_ASSERT_FALSE(rest_api_parse_id_from_uri("/api/v1/relays/5/extra",
                                                 "/api/v1/relays/",
                                                 &id));
    TEST_ASSERT_FALSE(rest_api_parse_id_from_uri("/api/v1/inputs/5", "/api/v1/relays/", &id));
}

TEST_CASE("rest_api device maps MOD-IO sync enums to wire strings",
          "[qa][rest_api][device]")
{
    TEST_ASSERT_EQUAL_STRING("absent", rest_api_modio_sync_to_string(REST_API_MODIO_SYNC_ABSENT));
    TEST_ASSERT_EQUAL_STRING("synchronized",
                             rest_api_modio_sync_to_string(REST_API_MODIO_SYNC_SYNCHRONIZED));
}

TEST_CASE("rest_api device fails closed when no auth handler is configured",
          "[qa][rest_api][device]")
{
    const rest_api_config_t config = {
        .port = 18081U,
        .auth_handler = NULL,
        .status_provider = status_provider,
    };

    ensure_tcpip_ready();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, rest_api_start(&config));
    TEST_ASSERT_NULL(rest_api_get_server());
}

TEST_CASE("rest_api device fails closed when no status provider is configured",
          "[qa][rest_api][device]")
{
    const rest_api_config_t config = {
        .port = 18080U,
        .auth_handler = allow_auth_handler,
        .status_provider = NULL,
    };

    ensure_tcpip_ready();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, rest_api_start(&config));
    TEST_ASSERT_NULL(rest_api_get_server());
}

TEST_CASE("rest_api device returns 401 when auth handler reports unauthorized",
          "[qa][rest_api][device]")
{
    static const uint16_t test_port = 18082U;
    static const rest_api_config_t config = {
        .port = test_port,
        .auth_handler = unauthorized_auth_handler,
        .status_provider = status_provider,
    };
    char response[512];

    ensure_tcpip_ready();
    TEST_ASSERT_EQUAL(ESP_OK, rest_api_start(&config));

    perform_status_request(test_port, NULL, response, sizeof(response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 401 Unauthorized"));
    TEST_ASSERT_NULL(strstr(response, "X-FW-Version: "));
    TEST_ASSERT_NULL(strstr(response, "X-ModIO-Present: "));
    TEST_ASSERT_NULL(strstr(response, "X-ModIO-Sync: "));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"code\":\"AUTH_REQUIRED\""));
}

TEST_CASE("rest_api device returns 403 when auth handler reports forbidden",
          "[qa][rest_api][device]")
{
    static const uint16_t test_port = 18083U;
    static const rest_api_config_t config = {
        .port = test_port,
        .auth_handler = forbidden_auth_handler,
        .status_provider = status_provider,
    };
    char response[512];

    ensure_tcpip_ready();
    TEST_ASSERT_EQUAL(ESP_OK, rest_api_start(&config));

    perform_status_request(test_port, NULL, response, sizeof(response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 403 Forbidden"));
    TEST_ASSERT_NULL(strstr(response, "X-FW-Version: "));
    TEST_ASSERT_NULL(strstr(response, "X-ModIO-Present: "));
    TEST_ASSERT_NULL(strstr(response, "X-ModIO-Sync: "));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"code\":\"AUTH_FORBIDDEN\""));
}

TEST_CASE("auth device generates a first-boot token and accepts it",
          "[firmware][auth][device]")
{
    static const uint16_t test_port = 18084U;
    static const rest_api_config_t config = {
        .port = test_port,
        .auth_handler = auth_check,
        .status_provider = status_provider,
    };
    auth_token_fixture_t fixture = {0};
    char authorization_header[DEVICE_CONFIG_API_TOKEN_MAX_LEN + 32U];
    char response[768];
    volatile bool fixture_captured = false;
    bool token_is_set = false;
    char generated_token[DEVICE_CONFIG_API_TOKEN_MAX_LEN + 1];

    ensure_tcpip_ready();
    if (TEST_PROTECT()) {
        capture_auth_token_fixture(&fixture);
        fixture_captured = true;

        TEST_ASSERT_EQUAL(ESP_OK, device_config_set_api_token(NULL, NULL));
        TEST_ASSERT_EQUAL(ESP_OK, auth_init());
        TEST_ASSERT_EQUAL(ESP_OK,
                          device_config_get_api_token(generated_token,
                                                      sizeof(generated_token),
                                                      &token_is_set));
        TEST_ASSERT_TRUE(token_is_set);
        TEST_ASSERT_EQUAL_UINT32(32U, strlen(generated_token));

        TEST_ASSERT_GREATER_THAN_INT32(0,
                                       snprintf(authorization_header,
                                                sizeof(authorization_header),
                                                "Authorization: Bearer %s",
                                                generated_token));
        TEST_ASSERT_EQUAL(ESP_OK, rest_api_start(&config));

        perform_status_request(test_port, authorization_header, response, sizeof(response));
        TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 200 OK"));
        TEST_ASSERT_NOT_NULL(strstr(response, "X-FW-Version: "));
        TEST_ASSERT_NOT_NULL(strstr(response, "X-ModIO-Present: true"));
        TEST_ASSERT_NOT_NULL(strstr(response, "X-ModIO-Sync: synchronized"));
        TEST_ASSERT_NOT_NULL(strstr(response, "\"hostname\":\"loopback-relay\""));
    }

    if (fixture_captured) {
        restore_auth_token_fixture(&fixture);
    }
}

TEST_CASE("auth device distinguishes missing and wrong bearer tokens",
          "[firmware][auth][device]")
{
    static const uint16_t test_port = 18085U;
    static const rest_api_config_t config = {
        .port = test_port,
        .auth_handler = auth_check,
        .status_provider = status_provider,
    };
    auth_token_fixture_t fixture = {0};
    char response[768];
    volatile bool fixture_captured = false;

    ensure_tcpip_ready();
    if (TEST_PROTECT()) {
        capture_auth_token_fixture(&fixture);
        fixture_captured = true;

        TEST_ASSERT_EQUAL(ESP_OK, device_config_set_api_token("loopback-secret", NULL));
        TEST_ASSERT_EQUAL(ESP_OK, auth_init());
        TEST_ASSERT_EQUAL(ESP_OK, rest_api_start(&config));

        perform_status_request(test_port, NULL, response, sizeof(response));
        TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 401 Unauthorized"));
        TEST_ASSERT_NULL(strstr(response, "X-FW-Version: "));
        TEST_ASSERT_NULL(strstr(response, "X-ModIO-Present: "));
        TEST_ASSERT_NULL(strstr(response, "X-ModIO-Sync: "));
        TEST_ASSERT_NOT_NULL(strstr(response, "\"code\":\"AUTH_REQUIRED\""));

        perform_status_request(test_port,
                               "Authorization: Bearer wrong-secret",
                               response,
                               sizeof(response));
        TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 403 Forbidden"));
        TEST_ASSERT_NULL(strstr(response, "X-FW-Version: "));
        TEST_ASSERT_NULL(strstr(response, "X-ModIO-Present: "));
        TEST_ASSERT_NULL(strstr(response, "X-ModIO-Sync: "));
        TEST_ASSERT_NOT_NULL(strstr(response, "\"code\":\"AUTH_FORBIDDEN\""));
    }

    if (fixture_captured) {
        restore_auth_token_fixture(&fixture);
    }
}

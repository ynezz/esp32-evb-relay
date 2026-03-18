#include <inttypes.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <sys/time.h>

#include "auth.h"
#include "board.h"
#include "device_config.h"
#include "esp_image_format.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input_monitor.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mod_io.h"
#include "ota.h"
#include "relay.h"
#include "relay_events.h"
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

static esp_err_t modio_absent_status_provider(rest_api_status_view_t *status, void *ctx)
{
    (void)ctx;

    TEST_ASSERT_NOT_NULL(status);
    status->network.connected = true;
    strncpy(status->network.hostname, "loopback-relay", sizeof(status->network.hostname) - 1U);
    strncpy(status->network.ip, "127.0.0.1", sizeof(status->network.ip) - 1U);
    strncpy(status->network.netmask, "255.0.0.0", sizeof(status->network.netmask) - 1U);
    strncpy(status->network.gateway, "127.0.0.1", sizeof(status->network.gateway) - 1U);
    status->modio_present = false;
    status->modio_sync = REST_API_MODIO_SYNC_ABSENT;
    return ESP_OK;
}

typedef struct {
    char api_token[DEVICE_CONFIG_API_TOKEN_MAX_LEN + 1];
    bool api_token_set;
} auth_token_fixture_t;

typedef struct {
    device_config_snapshot_t snapshot;
    char api_token[DEVICE_CONFIG_API_TOKEN_MAX_LEN + 1];
    bool api_token_set;
} device_config_fixture_t;

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

static void capture_device_config_fixture(device_config_fixture_t *fixture)
{
    TEST_ASSERT_NOT_NULL(fixture);
    TEST_ASSERT_EQUAL(ESP_OK, device_config_get_snapshot(&fixture->snapshot));
    TEST_ASSERT_EQUAL(ESP_OK,
                      device_config_get_api_token(fixture->api_token,
                                                  sizeof(fixture->api_token),
                                                  &fixture->api_token_set));
}

static void restore_device_config_fixture(const device_config_fixture_t *fixture)
{
    TEST_ASSERT_NOT_NULL(fixture);
    TEST_ASSERT_EQUAL(ESP_OK, device_config_set_poll_interval_ms(fixture->snapshot.poll_interval_ms, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, device_config_set_hostname(fixture->snapshot.hostname, NULL));
    TEST_ASSERT_EQUAL(ESP_OK,
                      device_config_set_modio_boot_policy(fixture->snapshot.modio_boot_policy, NULL));

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
    ssize_t n;
    size_t received;
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

    received = 0U;
    while (received < (response_size - 1U)) {
        n = recv(sock, response + received, response_size - received - 1U, 0);
        if (n <= 0) {
            break;
        }
        received += (size_t)n;
        response[received] = '\0';
    }
    TEST_ASSERT_GREATER_THAN_UINT32(0U, received);
    close(sock);
}

static size_t count_substring_occurrences(const char *haystack, const char *needle)
{
    const char *cursor;
    size_t count = 0U;
    size_t needle_len;

    TEST_ASSERT_NOT_NULL(haystack);
    TEST_ASSERT_NOT_NULL(needle);

    needle_len = strlen(needle);
    TEST_ASSERT_GREATER_THAN_UINT32(0U, (uint32_t)needle_len);

    cursor = haystack;
    while ((cursor = strstr(cursor, needle)) != NULL) {
        ++count;
        cursor += needle_len;
    }

    return count;
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

static int open_http_stream_request(uint16_t port,
                                    const char *path,
                                    const char *authorization_header)
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
    int written;
    size_t request_len = 0U;
    ssize_t sent;

    TEST_ASSERT_NOT_NULL(path);

    written = snprintf(request + request_len,
                       sizeof(request) - request_len,
                       "GET %s HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "Accept: text/event-stream\r\n",
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

    written = snprintf(request + request_len,
                       sizeof(request) - request_len,
                       "Connection: keep-alive\r\n"
                       "\r\n");
    TEST_ASSERT_GREATER_THAN_INT32(0, written);
    request_len += (size_t)written;
    TEST_ASSERT_LESS_THAN_UINT32(sizeof(request), request_len + 1U);

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
    return sock;
}

static void read_stream_until_contains(int sock,
                                       const char *needle,
                                       char *response,
                                       size_t response_size)
{
    size_t total = 0U;

    TEST_ASSERT_GREATER_OR_EQUAL_INT32(0, sock);
    TEST_ASSERT_NOT_NULL(needle);
    TEST_ASSERT_NOT_NULL(response);
    TEST_ASSERT_GREATER_THAN_UINT32(0U, response_size);

    memset(response, 0, response_size);
    while (total < (response_size - 1U)) {
        ssize_t received = recv(sock, response + total, response_size - total - 1U, 0);

        TEST_ASSERT_GREATER_THAN_INT32(0, received);
        total += (size_t)received;
        response[total] = '\0';
        if (strstr(response, needle) != NULL) {
            return;
        }
    }

    TEST_FAIL_MESSAGE("Expected stream fragment was not received");
}

static void read_response_to_close(int sock, char *response, size_t response_size)
{
    size_t total = 0U;

    TEST_ASSERT_GREATER_OR_EQUAL_INT32(0, sock);
    TEST_ASSERT_NOT_NULL(response);
    TEST_ASSERT_GREATER_THAN_UINT32(0U, response_size);

    memset(response, 0, response_size);
    while (total < (response_size - 1U)) {
        ssize_t received = recv(sock, response + total, response_size - total - 1U, 0);

        if (received <= 0) {
            break;
        }

        total += (size_t)received;
        response[total] = '\0';
    }

    TEST_ASSERT_GREATER_THAN_UINT32(0U, total);
}

static size_t get_running_app_image_size(void)
{
    const esp_partition_t *running_partition = esp_ota_get_running_partition();
    esp_partition_pos_t partition_pos = {0};
    esp_image_metadata_t metadata = {0};

    TEST_ASSERT_NOT_NULL(running_partition);
    partition_pos.offset = running_partition->address;
    partition_pos.size = running_partition->size;
    TEST_ASSERT_EQUAL(ESP_OK, esp_image_get_metadata(&partition_pos, &metadata));
    return metadata.image_len;
}

static void perform_partition_upload_request(uint16_t port,
                                             const char *path,
                                             const char *authorization_header,
                                             const esp_partition_t *partition,
                                             size_t image_size,
                                             char *response,
                                             size_t response_size)
{
    char request[768];
    uint8_t chunk[1024];
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
    int written;
    size_t request_len = 0U;
    size_t offset = 0U;
    ssize_t sent;

    TEST_ASSERT_NOT_NULL(path);
    TEST_ASSERT_NOT_NULL(partition);

    written = snprintf(request + request_len,
                       sizeof(request) - request_len,
                       "POST %s HTTP/1.1\r\n"
                       "Host: localhost\r\n"
                       "Content-Type: application/octet-stream\r\n"
                       "Content-Length: %u\r\n",
                       path,
                       (unsigned)image_size);
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

    written = snprintf(request + request_len,
                       sizeof(request) - request_len,
                       "Connection: close\r\n"
                       "\r\n");
    TEST_ASSERT_GREATER_THAN_INT32(0, written);
    request_len += (size_t)written;
    TEST_ASSERT_LESS_THAN_UINT32(sizeof(request), request_len + 1U);

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

    while (offset < image_size) {
        size_t chunk_len = image_size - offset;

        if (chunk_len > sizeof(chunk)) {
            chunk_len = sizeof(chunk);
        }

        TEST_ASSERT_EQUAL(ESP_OK, esp_partition_read(partition, offset, chunk, chunk_len));
        sent = send(sock, chunk, chunk_len, 0);
        TEST_ASSERT_EQUAL_INT((int)chunk_len, sent);
        offset += chunk_len;
    }

    read_response_to_close(sock, response, response_size);
    close(sock);
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

TEST_CASE("rest_api device exposes combined and MOD-IO relay endpoints",
          "[qa][rest_api][device]")
{
    static const uint16_t test_port = 18090U;
    static const rest_api_config_t config = {
        .port = test_port,
        .auth_handler = allow_auth_handler,
        .status_provider = status_provider,
    };
    char response[2048];
    uint8_t relay_mask = 0;
    mod_io_relay_sync_t relay_sync = MOD_IO_RELAY_SYNC_ABSENT;

    ensure_tcpip_ready();
    TEST_ASSERT_EQUAL(ESP_OK, board_init());
    TEST_ASSERT_EQUAL(ESP_OK, relay_init());
    TEST_ASSERT_EQUAL(ESP_OK, mod_io_init(board_i2c_bus_handle()));
    TEST_ASSERT_EQUAL(ESP_OK, mod_io_probe());
    TEST_ASSERT_EQUAL(ESP_OK, mod_io_set_relays(0x00U));
    TEST_ASSERT_EQUAL(ESP_OK, rest_api_start(&config));

    perform_http_request(test_port, "GET", "/api/v1/relays", NULL, NULL, response, sizeof(response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 200 OK"));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"modio_present\":true"));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"modio_sync\":\"synchronized\""));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"group\":\"onboard\",\"id\":1,\"state\":false,\"sync\":null"));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"group\":\"onboard\",\"id\":2,\"state\":false,\"sync\":null"));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"group\":\"modio\",\"id\":1,\"state\":false,\"sync\":\"synchronized\""));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"group\":\"modio\",\"id\":4,\"state\":false,\"sync\":\"synchronized\""));

    perform_http_request(test_port,
                         "PUT",
                         "/api/v1/relays/modio/1",
                         NULL,
                         "{\"state\":true}",
                         response,
                         sizeof(response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 200 OK"));
    TEST_ASSERT_NOT_NULL(strstr(response,
                                "\"relay\":{\"group\":\"modio\",\"id\":1,\"state\":true,\"sync\":\"synchronized\"}"));
    TEST_ASSERT_EQUAL(ESP_OK, mod_io_get_relays(&relay_mask, &relay_sync));
    TEST_ASSERT_EQUAL_HEX8(0x01U, relay_mask);
    TEST_ASSERT_EQUAL_INT(MOD_IO_RELAY_SYNC_SYNCHRONIZED, relay_sync);

    perform_http_request(test_port,
                         "POST",
                         "/api/v1/relays/modio/1/toggle",
                         NULL,
                         NULL,
                         response,
                         sizeof(response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 200 OK"));
    TEST_ASSERT_NOT_NULL(strstr(response,
                                "\"relay\":{\"group\":\"modio\",\"id\":1,\"state\":false,\"sync\":\"synchronized\"}"));
    TEST_ASSERT_EQUAL(ESP_OK, mod_io_get_relays(&relay_mask, &relay_sync));
    TEST_ASSERT_EQUAL_HEX8(0x00U, relay_mask);
    TEST_ASSERT_EQUAL_INT(MOD_IO_RELAY_SYNC_SYNCHRONIZED, relay_sync);

    perform_http_request(test_port,
                         "PUT",
                         "/api/v1/relays/modio",
                         NULL,
                         "{\"states\":[true,false,true,false]}",
                         response,
                         sizeof(response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 200 OK"));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"group\":\"modio\",\"id\":1,\"state\":true,\"sync\":\"synchronized\""));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"group\":\"modio\",\"id\":2,\"state\":false,\"sync\":\"synchronized\""));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"group\":\"modio\",\"id\":3,\"state\":true,\"sync\":\"synchronized\""));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"group\":\"modio\",\"id\":4,\"state\":false,\"sync\":\"synchronized\""));
    TEST_ASSERT_EQUAL(ESP_OK, mod_io_get_relays(&relay_mask, &relay_sync));
    TEST_ASSERT_EQUAL_HEX8(0x05U, relay_mask);
    TEST_ASSERT_EQUAL_INT(MOD_IO_RELAY_SYNC_SYNCHRONIZED, relay_sync);

    perform_http_request(test_port, "GET", "/api/v1/relays/modio", NULL, NULL, response, sizeof(response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 200 OK"));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"group\":\"modio\",\"id\":1,\"state\":true,\"sync\":\"synchronized\""));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"group\":\"modio\",\"id\":2,\"state\":false,\"sync\":\"synchronized\""));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"group\":\"modio\",\"id\":3,\"state\":true,\"sync\":\"synchronized\""));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"group\":\"modio\",\"id\":4,\"state\":false,\"sync\":\"synchronized\""));
}

TEST_CASE("rest_api device reports absent MOD-IO on relay routes", "[qa][rest_api][device]")
{
    static const uint16_t test_port = 18091U;
    static const rest_api_config_t config = {
        .port = test_port,
        .auth_handler = allow_auth_handler,
        .status_provider = modio_absent_status_provider,
    };
    char response[1536];

    ensure_tcpip_ready();
    TEST_ASSERT_EQUAL(ESP_OK, board_init());
    TEST_ASSERT_EQUAL(ESP_OK, relay_init());
    TEST_ASSERT_EQUAL(ESP_OK, rest_api_start(&config));

    perform_http_request(test_port, "GET", "/api/v1/relays", NULL, NULL, response, sizeof(response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 200 OK"));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"modio_present\":false"));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"modio_sync\":\"absent\""));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"group\":\"onboard\",\"id\":1,\"state\":false,\"sync\":null"));
    TEST_ASSERT_NULL(strstr(response, "\"group\":\"modio\""));

    perform_http_request(test_port, "GET", "/api/v1/relays/modio", NULL, NULL, response, sizeof(response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 503 Service Unavailable"));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"code\":\"MODIO_NOT_PRESENT\""));
    TEST_ASSERT_NOT_NULL(strstr(response, "X-ModIO-Present: false"));
    TEST_ASSERT_NOT_NULL(strstr(response, "X-ModIO-Sync: absent"));

    perform_http_request(test_port,
                         "POST",
                         "/api/v1/relays/modio/1/toggle",
                         NULL,
                         NULL,
                         response,
                         sizeof(response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 503 Service Unavailable"));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"code\":\"MODIO_NOT_PRESENT\""));

    perform_http_request(test_port,
                         "PUT",
                         "/api/v1/relays/modio/1",
                         NULL,
                         "{\"state\":true}",
                         response,
                         sizeof(response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 503 Service Unavailable"));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"code\":\"MODIO_NOT_PRESENT\""));
}

TEST_CASE("rest_api device exposes redacted config snapshots", "[qa][rest_api][device]")
{
    static const uint16_t test_port = 18088U;
    static const rest_api_config_t config = {
        .port = test_port,
        .auth_handler = allow_auth_handler,
        .status_provider = status_provider,
    };
    device_config_fixture_t fixture = {0};
    char response[1024];
    volatile bool fixture_captured = false;

    ensure_tcpip_ready();
    if (TEST_PROTECT()) {
        capture_device_config_fixture(&fixture);
        fixture_captured = true;

        TEST_ASSERT_EQUAL(ESP_OK, device_config_set_poll_interval_ms(250U, NULL));
        TEST_ASSERT_EQUAL(ESP_OK, device_config_set_hostname("lab-relay", NULL));
        TEST_ASSERT_EQUAL(ESP_OK,
                          device_config_set_modio_boot_policy(DEVICE_CONFIG_MODIO_BOOT_POLICY_ALL_OFF,
                                                              NULL));
        TEST_ASSERT_EQUAL(ESP_OK, device_config_set_api_token("loopback-secret", NULL));

        TEST_ASSERT_EQUAL(ESP_OK, rest_api_start(&config));
        perform_http_request(test_port, "GET", "/api/v1/config", NULL, NULL, response, sizeof(response));

        TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 200 OK"));
        TEST_ASSERT_NOT_NULL(strstr(response, "X-FW-Version: "));
        TEST_ASSERT_NOT_NULL(strstr(response, "\"poll_interval_ms\":250"));
        TEST_ASSERT_NOT_NULL(strstr(response, "\"hostname\":\"lab-relay\""));
        TEST_ASSERT_NOT_NULL(strstr(response, "\"modio_boot_policy\":\"all_off\""));
        TEST_ASSERT_NOT_NULL(strstr(response, "\"api_token_set\":true"));
        TEST_ASSERT_NULL(strstr(response, "loopback-secret"));
    }

    if (fixture_captured) {
        restore_device_config_fixture(&fixture);
    }
}

TEST_CASE("rest_api device updates config with live apply metadata",
          "[qa][rest_api][device]")
{
    static const uint16_t test_port = 18089U;
    static const rest_api_config_t config = {
        .port = test_port,
        .auth_handler = allow_auth_handler,
        .status_provider = status_provider,
    };
    device_config_fixture_t fixture = {0};
    device_config_snapshot_t snapshot;
    char api_token[DEVICE_CONFIG_API_TOKEN_MAX_LEN + 1];
    bool api_token_set = false;
    char response[1536];
    volatile bool fixture_captured = false;

    ensure_tcpip_ready();
    if (TEST_PROTECT()) {
        capture_device_config_fixture(&fixture);
        fixture_captured = true;

        TEST_ASSERT_EQUAL(ESP_OK, device_config_set_poll_interval_ms(100U, NULL));
        TEST_ASSERT_EQUAL(ESP_OK, device_config_set_hostname("esp32-evb-relay", NULL));
        TEST_ASSERT_EQUAL(ESP_OK,
                          device_config_set_modio_boot_policy(
                              DEVICE_CONFIG_MODIO_BOOT_POLICY_LEAVE_UNCHANGED,
                              NULL));
        TEST_ASSERT_EQUAL(ESP_OK, device_config_set_api_token(NULL, NULL));

        TEST_ASSERT_EQUAL(ESP_OK, rest_api_start(&config));
        perform_http_request(test_port,
                             "PUT",
                             "/api/v1/config",
                             NULL,
                             "{\"poll_interval_ms\":250,\"hostname\":\"lab-relay\","
                             "\"modio_boot_policy\":\"all_off\","
                             "\"api_token\":\"updated-secret\"}",
                             response,
                             sizeof(response));

        TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 200 OK"));
        TEST_ASSERT_NOT_NULL(strstr(response, "\"key\":\"api_token_set\",\"old\":false,\"new\":true,\"live\":true"));
        TEST_ASSERT_NOT_NULL(strstr(response, "\"key\":\"poll_interval_ms\",\"old\":100,\"new\":250,\"live\":true"));
        TEST_ASSERT_NOT_NULL(strstr(response, "\"key\":\"hostname\",\"old\":\"esp32-evb-relay\",\"new\":\"lab-relay\",\"live\":false"));
        TEST_ASSERT_NOT_NULL(strstr(response, "\"key\":\"modio_boot_policy\",\"old\":\"leave_unchanged\",\"new\":\"all_off\",\"live\":false"));
        TEST_ASSERT_NOT_NULL(strstr(response, "\"restart_required\":true"));
        TEST_ASSERT_NULL(strstr(response, "updated-secret"));

        TEST_ASSERT_EQUAL(ESP_OK, device_config_get_snapshot(&snapshot));
        TEST_ASSERT_EQUAL_UINT32(250U, snapshot.poll_interval_ms);
        TEST_ASSERT_EQUAL_STRING("lab-relay", snapshot.hostname);
        TEST_ASSERT_EQUAL(DEVICE_CONFIG_MODIO_BOOT_POLICY_ALL_OFF, snapshot.modio_boot_policy);
        TEST_ASSERT_TRUE(snapshot.api_token_set);

        TEST_ASSERT_EQUAL(ESP_OK,
                          device_config_get_api_token(api_token, sizeof(api_token), &api_token_set));
        TEST_ASSERT_TRUE(api_token_set);
        TEST_ASSERT_EQUAL_STRING("updated-secret", api_token);
    }

    if (fixture_captured) {
        restore_device_config_fixture(&fixture);
    }
}

TEST_CASE("rest_api device rejects invalid config updates without partial apply",
          "[qa][rest_api][device]")
{
    static const uint16_t test_port = 18090U;
    static const rest_api_config_t config = {
        .port = test_port,
        .auth_handler = allow_auth_handler,
        .status_provider = status_provider,
    };
    device_config_fixture_t fixture = {0};
    device_config_snapshot_t snapshot;
    char response[1024];
    volatile bool fixture_captured = false;

    ensure_tcpip_ready();
    if (TEST_PROTECT()) {
        capture_device_config_fixture(&fixture);
        fixture_captured = true;

        TEST_ASSERT_EQUAL(ESP_OK, device_config_set_poll_interval_ms(100U, NULL));
        TEST_ASSERT_EQUAL(ESP_OK, device_config_set_hostname("esp32-evb-relay", NULL));
        TEST_ASSERT_EQUAL(ESP_OK,
                          device_config_set_modio_boot_policy(
                              DEVICE_CONFIG_MODIO_BOOT_POLICY_LEAVE_UNCHANGED,
                              NULL));
        TEST_ASSERT_EQUAL(ESP_OK, rest_api_start(&config));

        perform_http_request(test_port,
                             "PUT",
                             "/api/v1/config",
                             NULL,
                             "{\"poll_interval_ms\":250,\"modio_boot_policy\":\"bogus\"}",
                             response,
                             sizeof(response));

        TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 400 Bad Request"));
        TEST_ASSERT_NOT_NULL(strstr(response, "\"code\":\"INVALID_CONFIG_VALUE\""));

        TEST_ASSERT_EQUAL(ESP_OK, device_config_get_snapshot(&snapshot));
        TEST_ASSERT_EQUAL_UINT32(100U, snapshot.poll_interval_ms);
        TEST_ASSERT_EQUAL_STRING("esp32-evb-relay", snapshot.hostname);
        TEST_ASSERT_EQUAL(DEVICE_CONFIG_MODIO_BOOT_POLICY_LEAVE_UNCHANGED, snapshot.modio_boot_policy);
    }

    if (fixture_captured) {
        restore_device_config_fixture(&fixture);
    }
}

TEST_CASE("rest_api device reports input sample unavailable before the monitor starts",
          "[qa][rest_api][device]")
{
    static const uint16_t test_port = 18092U;
    static const char *paths[] = {
        "/api/v1/inputs/digital",
        "/api/v1/inputs/digital/1",
        "/api/v1/inputs/analog",
        "/api/v1/inputs/analog/1",
    };
    static const rest_api_config_t config = {
        .port = test_port,
        .auth_handler = allow_auth_handler,
        .status_provider = status_provider,
    };
    char response[1024];

    ensure_tcpip_ready();
    TEST_ASSERT_EQUAL(ESP_OK, rest_api_start(&config));

    for (size_t index = 0; index < (sizeof(paths) / sizeof(paths[0])); ++index) {
        perform_http_request(test_port, "GET", paths[index], NULL, NULL, response, sizeof(response));

        TEST_ASSERT_EQUAL_UINT32(1U,
                                 (uint32_t)count_substring_occurrences(response, "HTTP/1.1 "));
        TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 503 Service Unavailable"));
        TEST_ASSERT_NOT_NULL(strstr(response, "\"code\":\"MODIO_SAMPLE_UNAVAILABLE\""));
        TEST_ASSERT_NOT_NULL(strstr(response, "\"retryable\":true"));
        TEST_ASSERT_NOT_NULL(strstr(response, "X-ModIO-Present: true"));
        TEST_ASSERT_NOT_NULL(strstr(response, "X-ModIO-Sync: synchronized"));
    }
}

TEST_CASE("rest_api device reports absent MOD-IO on input routes", "[qa][rest_api][device]")
{
    static const uint16_t test_port = 18093U;
    static const char *paths[] = {
        "/api/v1/inputs/digital",
        "/api/v1/inputs/digital/1",
        "/api/v1/inputs/analog",
        "/api/v1/inputs/analog/1",
    };
    static const rest_api_config_t config = {
        .port = test_port,
        .auth_handler = allow_auth_handler,
        .status_provider = modio_absent_status_provider,
    };
    char response[1024];

    ensure_tcpip_ready();
    TEST_ASSERT_EQUAL(ESP_OK, rest_api_start(&config));

    for (size_t index = 0; index < (sizeof(paths) / sizeof(paths[0])); ++index) {
        perform_http_request(test_port, "GET", paths[index], NULL, NULL, response, sizeof(response));

        TEST_ASSERT_EQUAL_UINT32(1U,
                                 (uint32_t)count_substring_occurrences(response, "HTTP/1.1 "));
        TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 503 Service Unavailable"));
        TEST_ASSERT_NOT_NULL(strstr(response, "\"code\":\"MODIO_NOT_PRESENT\""));
        TEST_ASSERT_NOT_NULL(strstr(response, "X-ModIO-Present: false"));
        TEST_ASSERT_NOT_NULL(strstr(response, "X-ModIO-Sync: absent"));
    }
}

TEST_CASE("rest_api device exposes cached digital and analog input snapshots",
          "[qa][rest_api][device]")
{
    static const uint16_t test_port = 18094U;
    static const rest_api_config_t config = {
        .port = test_port,
        .auth_handler = allow_auth_handler,
        .status_provider = status_provider,
    };
    device_config_fixture_t fixture = {0};
    input_monitor_snapshot_t snapshot = {0};
    char response[2048];
    char expected_fragment[128];
    volatile bool fixture_captured = false;

    ensure_tcpip_ready();
    if (TEST_PROTECT()) {
        capture_device_config_fixture(&fixture);
        fixture_captured = true;

        TEST_ASSERT_EQUAL(ESP_OK, device_config_set_poll_interval_ms(10000U, NULL));
        TEST_ASSERT_EQUAL(ESP_OK, board_init());
        TEST_ASSERT_EQUAL(ESP_OK, mod_io_init(board_i2c_bus_handle()));
        TEST_ASSERT_EQUAL(ESP_OK, mod_io_probe());
        TEST_ASSERT_EQUAL(ESP_OK, input_monitor_start());
        TEST_ASSERT_EQUAL(ESP_OK, input_monitor_poll_once_for_testing());
        TEST_ASSERT_EQUAL(ESP_OK, input_monitor_get_snapshot(&snapshot));
        TEST_ASSERT_TRUE(snapshot.modio_present);
        TEST_ASSERT_TRUE(snapshot.sample_valid);

        TEST_ASSERT_EQUAL(ESP_OK, rest_api_start(&config));

        perform_http_request(test_port, "GET", "/api/v1/inputs/digital", NULL, NULL, response, sizeof(response));
        TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 200 OK"));
        TEST_ASSERT_NOT_NULL(strstr(response, "X-ModIO-Present: true"));
        TEST_ASSERT_NOT_NULL(strstr(response, "X-ModIO-Sync: synchronized"));
        TEST_ASSERT_NOT_NULL(strstr(response, "\"staleness_ms\":"));
        TEST_ASSERT_NOT_NULL(strstr(response, "\"poll_interval_ms\":10000"));
        TEST_ASSERT_NOT_NULL(strstr(response, "\"sample_ts_ms\":"));
        for (uint8_t input_id = 1U; input_id <= MOD_IO_DIGITAL_INPUT_COUNT; ++input_id) {
            TEST_ASSERT_GREATER_THAN_INT32(
                0,
                snprintf(expected_fragment,
                         sizeof(expected_fragment),
                         "\"id\":%u,\"state\":%s",
                         (unsigned)input_id,
                         (snapshot.digital_mask & (uint8_t)(1U << (input_id - 1U))) != 0U ? "true"
                         : "false"));
            TEST_ASSERT_NOT_NULL(strstr(response, expected_fragment));
        }

        TEST_ASSERT_GREATER_THAN_INT32(0,
                                       snprintf(expected_fragment,
                                                sizeof(expected_fragment),
                                                "\"sample_ts_ms\":%" PRIu64,
                                                snapshot.sample_ts_ms));
        TEST_ASSERT_NOT_NULL(strstr(response, expected_fragment));

        perform_http_request(test_port,
                             "GET",
                             "/api/v1/inputs/digital/1",
                             NULL,
                             NULL,
                             response,
                             sizeof(response));
        TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 200 OK"));
        TEST_ASSERT_GREATER_THAN_INT32(
            0,
            snprintf(expected_fragment,
                     sizeof(expected_fragment),
                     "\"input\":{\"id\":1,\"state\":%s}",
                     (snapshot.digital_mask & 0x01U) != 0U ? "true" : "false"));
        TEST_ASSERT_NOT_NULL(strstr(response, expected_fragment));

        perform_http_request(test_port, "GET", "/api/v1/inputs/analog", NULL, NULL, response, sizeof(response));
        TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 200 OK"));
        TEST_ASSERT_NOT_NULL(strstr(response, "\"staleness_ms\":"));
        TEST_ASSERT_NOT_NULL(strstr(response, "\"poll_interval_ms\":10000"));
        for (uint8_t input_id = 1U; input_id <= MOD_IO_ANALOG_INPUT_COUNT; ++input_id) {
            TEST_ASSERT_GREATER_THAN_INT32(
                0,
                snprintf(expected_fragment,
                         sizeof(expected_fragment),
                         "\"id\":%u,\"value\":%u",
                         (unsigned)input_id,
                         (unsigned)snapshot.analog_values[input_id - 1U]));
            TEST_ASSERT_NOT_NULL(strstr(response, expected_fragment));
        }

        perform_http_request(test_port,
                             "GET",
                             "/api/v1/inputs/analog/1",
                             NULL,
                             NULL,
                             response,
                             sizeof(response));
        TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 200 OK"));
        TEST_ASSERT_GREATER_THAN_INT32(0,
                                       snprintf(expected_fragment,
                                                sizeof(expected_fragment),
                                                "\"input\":{\"id\":1,\"value\":%u}",
                                                (unsigned)snapshot.analog_values[0]));
        TEST_ASSERT_NOT_NULL(strstr(response, expected_fragment));

        perform_http_request(test_port,
                             "GET",
                             "/api/v1/inputs/digital/9",
                             NULL,
                             NULL,
                             response,
                             sizeof(response));
        TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 404 Not Found"));
        TEST_ASSERT_NOT_NULL(strstr(response, "\"code\":\"INPUT_NOT_FOUND\""));
    }

    if (fixture_captured) {
        restore_device_config_fixture(&fixture);
    }
}

TEST_CASE("rest_api device streams relay events over SSE", "[qa][rest_api][device]")
{
    static const uint16_t test_port = 18095U;
    static const rest_api_config_t config = {
        .port = test_port,
        .auth_handler = allow_auth_handler,
        .status_provider = status_provider,
    };
    evb_relay_relay_changed_event_t event = {
        .group = EVB_RELAY_RELAY_GROUP_ONBOARD,
        .id = 1U,
        .state = true,
        .ts_ms = 12345U,
    };
    char response[1024];
    int sock = -1;

    ensure_tcpip_ready();
    TEST_ASSERT_EQUAL(ESP_OK, rest_api_start(&config));

    sock = open_http_stream_request(test_port, "/api/v1/events", NULL);
    read_stream_until_contains(sock, ":connected", response, sizeof(response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 200 OK"));
    TEST_ASSERT_NOT_NULL(strstr(response, "Content-Type: text/event-stream"));
    TEST_ASSERT_NOT_NULL(strstr(response, "X-ModIO-Present: true"));
    TEST_ASSERT_NOT_NULL(strstr(response, "X-ModIO-Sync: synchronized"));

    TEST_ASSERT_EQUAL(ESP_OK,
                      esp_event_post(EVB_RELAY_EVENT,
                                     EVB_RELAY_EVENT_RELAY_CHANGED,
                                     &event,
                                     sizeof(event),
                                     portMAX_DELAY));
    read_stream_until_contains(sock, "event: relay_changed", response, sizeof(response));
    TEST_ASSERT_NOT_NULL(strstr(response, "event: relay_changed"));
    TEST_ASSERT_NOT_NULL(strstr(response,
                                "data: {\"group\":\"onboard\",\"id\":1,\"state\":true,\"ts_ms\":12345}"));

    close(sock);
}

TEST_CASE("rest_api device streams MOD-IO presence events over SSE", "[qa][rest_api][device]")
{
    static const uint16_t test_port = 18099U;
    static const rest_api_config_t config = {
        .port = test_port,
        .auth_handler = allow_auth_handler,
        .status_provider = status_provider,
    };
    evb_relay_modio_presence_event_t event = {
        .present = false,
        .ts_ms = 12345U,
    };
    char response[1024];
    int sock = -1;

    ensure_tcpip_ready();
    TEST_ASSERT_EQUAL(ESP_OK, rest_api_start(&config));

    sock = open_http_stream_request(test_port, "/api/v1/events", NULL);
    read_stream_until_contains(sock, ":connected", response, sizeof(response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 200 OK"));
    TEST_ASSERT_NOT_NULL(strstr(response, "Content-Type: text/event-stream"));

    TEST_ASSERT_EQUAL(ESP_OK,
                      esp_event_post(EVB_RELAY_EVENT,
                                     EVB_RELAY_EVENT_MODIO_PRESENCE,
                                     &event,
                                     sizeof(event),
                                     portMAX_DELAY));
    read_stream_until_contains(sock, "event: modio_presence", response, sizeof(response));
    TEST_ASSERT_NOT_NULL(strstr(response, "event: modio_presence"));
    TEST_ASSERT_NOT_NULL(strstr(response, "data: {\"present\":false,\"ts_ms\":12345}"));

    close(sock);
}

TEST_CASE("rest_api device emits SSE heartbeats", "[qa][rest_api][device]")
{
    static const uint16_t test_port = 18096U;
    static const rest_api_config_t config = {
        .port = test_port,
        .auth_handler = allow_auth_handler,
        .status_provider = status_provider,
    };
    char response[1024];
    int sock = -1;

    ensure_tcpip_ready();
    TEST_ASSERT_EQUAL(ESP_OK, rest_api_start(&config));

    sock = open_http_stream_request(test_port, "/api/v1/events", NULL);
    read_stream_until_contains(sock, ":connected", response, sizeof(response));
    read_stream_until_contains(sock, ":heartbeat", response, sizeof(response));
    TEST_ASSERT_NOT_NULL(strstr(response, ":heartbeat"));

    close(sock);
}

TEST_CASE("rest_api device enforces the SSE client limit", "[qa][rest_api][device]")
{
    static const uint16_t test_port = 18097U;
    static const size_t max_sse_clients = 4U;
    static const rest_api_config_t config = {
        .port = test_port,
        .auth_handler = allow_auth_handler,
        .status_provider = status_provider,
    };
    char response[1024];
    int sockets[4];
    int extra_sock = -1;

    memset(sockets, 0xFF, sizeof(sockets));

    ensure_tcpip_ready();
    TEST_ASSERT_EQUAL(ESP_OK, rest_api_start(&config));

    for (size_t index = 0; index < max_sse_clients; ++index) {
        sockets[index] = open_http_stream_request(test_port, "/api/v1/events", NULL);
        read_stream_until_contains(sockets[index], ":connected", response, sizeof(response));
        TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 200 OK"));
    }

    extra_sock = open_http_stream_request(test_port, "/api/v1/events", NULL);
    read_stream_until_contains(extra_sock,
                               "\"code\":\"SSE_CLIENT_LIMIT_REACHED\"",
                               response,
                               sizeof(response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 503 Service Unavailable"));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"code\":\"SSE_CLIENT_LIMIT_REACHED\""));
    close(extra_sock);

    for (size_t index = 0; index < max_sse_clients; ++index) {
        close(sockets[index]);
    }
}

TEST_CASE("rest_api device returns 503 when SSE task startup fails after async detach",
          "[qa][rest_api][device]")
{
    static const uint16_t test_port = 18098U;
    static const rest_api_config_t config = {
        .port = test_port,
        .auth_handler = allow_auth_handler,
        .status_provider = status_provider,
    };
    char response[1024];
    int sock = -1;

    ensure_tcpip_ready();
    TEST_ASSERT_EQUAL(ESP_OK, rest_api_start(&config));

    rest_api_sse_force_next_client_task_create_failure_for_testing();

    sock = open_http_stream_request(test_port, "/api/v1/events", NULL);
    read_stream_until_contains(sock, "\"code\":\"SSE_UNAVAILABLE\"", response, sizeof(response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 503 Service Unavailable"));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"code\":\"SSE_UNAVAILABLE\""));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"message\":\"Failed to start event stream task\""));
    close(sock);

    sock = open_http_stream_request(test_port, "/api/v1/events", NULL);
    read_stream_until_contains(sock, ":connected", response, sizeof(response));
    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 200 OK"));
    close(sock);
}

TEST_CASE("rest_api device stop owns SSE dispatch task deletion", "[qa][rest_api][device]")
{
    static const uint16_t test_port = 18100U;
    static const rest_api_config_t config = {
        .port = test_port,
        .auth_handler = allow_auth_handler,
        .status_provider = status_provider,
    };
    char response[1024];
    int sock = -1;

    ensure_tcpip_ready();
    TEST_ASSERT_EQUAL(ESP_OK, rest_api_start(&config));

    sock = open_http_stream_request(test_port, "/api/v1/events", NULL);
    read_stream_until_contains(sock, ":connected", response, sizeof(response));
    rest_api_sse_hold_dispatch_task_on_shutdown_for_testing(true);
    TEST_ASSERT_EQUAL(ESP_OK, rest_api_stop());
    TEST_ASSERT_TRUE(rest_api_sse_dispatch_task_deleted_by_stop_for_testing());

    close(sock);
}

TEST_CASE("rest_api device accepts OTA uploads and switches the boot partition",
          "[qa][rest_api][device]")
{
    static const uint16_t test_port = 18098U;
    static const rest_api_config_t config = {
        .port = test_port,
        .auth_handler = allow_auth_handler,
        .status_provider = status_provider,
    };
    const esp_partition_t *running_partition = esp_ota_get_running_partition();
    const esp_partition_t *boot_partition;
    char response[1024];
    char expected_bytes[64];
    size_t image_size;

    ensure_tcpip_ready();
    TEST_ASSERT_NOT_NULL(running_partition);
    TEST_ASSERT_EQUAL(ESP_OK, rest_api_start(&config));

    image_size = get_running_app_image_size();
    perform_partition_upload_request(test_port,
                                     "/api/v1/ota",
                                     NULL,
                                     running_partition,
                                     image_size,
                                     response,
                                     sizeof(response));

    TEST_ASSERT_NOT_NULL(strstr(response, "HTTP/1.1 200 OK"));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"partition\":\"ota_"));
    TEST_ASSERT_NOT_NULL(strstr(response, "\"reboot_delay_ms\":2000"));
    snprintf(expected_bytes,
             sizeof(expected_bytes),
             "\"bytes_received\":%u",
             (unsigned)image_size);
    TEST_ASSERT_NOT_NULL(strstr(response, expected_bytes));
    TEST_ASSERT_TRUE(ota_reboot_scheduled_for_testing());

    boot_partition = esp_ota_get_boot_partition();
    TEST_ASSERT_NOT_NULL(boot_partition);
    TEST_ASSERT_NOT_EQUAL(running_partition, boot_partition);
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

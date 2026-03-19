#include "rest_api.h"
#include "rest_api_request_recv.h"

#include <string.h>

#include "unity.h"

typedef struct httpd_req {
    const int *responses;
    size_t response_count;
    size_t call_count;
    const char *payload;
    size_t payload_offset;
} httpd_req;

int httpd_req_recv(httpd_req_t *req, char *buf, size_t buf_len)
{
    httpd_req *mock_req = (httpd_req *)req;
    int response;

    TEST_ASSERT_NOT_NULL(mock_req);
    TEST_ASSERT_TRUE(mock_req->call_count < mock_req->response_count);

    response = mock_req->responses[mock_req->call_count++];
    if (response > 0) {
        TEST_ASSERT_NOT_NULL(mock_req->payload);
        TEST_ASSERT_TRUE((size_t)response <= buf_len);
        memcpy(buf, mock_req->payload + mock_req->payload_offset, (size_t)response);
        mock_req->payload_offset += (size_t)response;
    }

    return response;
}

static void test_rest_api_modio_sync_from_driver_maps_absent(void)
{
    TEST_ASSERT_EQUAL_INT(REST_API_MODIO_SYNC_ABSENT,
                          rest_api_modio_sync_from_driver(MOD_IO_RELAY_SYNC_ABSENT));
}

static void test_rest_api_modio_sync_from_driver_maps_synchronized(void)
{
    TEST_ASSERT_EQUAL_INT(REST_API_MODIO_SYNC_SYNCHRONIZED,
                          rest_api_modio_sync_from_driver(MOD_IO_RELAY_SYNC_SYNCHRONIZED));
}

static void test_rest_api_modio_sync_from_driver_maps_unknown_to_absent(void)
{
    TEST_ASSERT_EQUAL_INT(REST_API_MODIO_SYNC_ABSENT,
                          rest_api_modio_sync_from_driver((mod_io_relay_sync_t)99));
}

static void test_rest_api_network_transport_to_string_maps_known_values(void)
{
    TEST_ASSERT_EQUAL_STRING("none",
                             rest_api_network_transport_to_string(REST_API_NETWORK_TRANSPORT_NONE));
    TEST_ASSERT_EQUAL_STRING("ethernet",
                             rest_api_network_transport_to_string(REST_API_NETWORK_TRANSPORT_ETHERNET));
    TEST_ASSERT_EQUAL_STRING("wifi",
                             rest_api_network_transport_to_string(REST_API_NETWORK_TRANSPORT_WIFI));
}

static void test_rest_api_network_transport_to_string_maps_unknown_to_none(void)
{
    TEST_ASSERT_EQUAL_STRING("none",
                             rest_api_network_transport_to_string((rest_api_network_transport_t)99));
}

static void test_rest_api_request_recv_exact_times_out_after_three_consecutive_timeouts(void)
{
    static const int responses[] = {
        HTTPD_SOCK_ERR_TIMEOUT,
        HTTPD_SOCK_ERR_TIMEOUT,
        HTTPD_SOCK_ERR_TIMEOUT,
        1,
    };
    httpd_req req = {
        .responses = responses,
        .response_count = sizeof(responses) / sizeof(responses[0]),
        .payload = "x",
    };
    char buffer[2] = {0};
    size_t received = 99U;

    TEST_ASSERT_EQUAL_INT(ESP_ERR_TIMEOUT,
                          rest_api_request_recv_exact((httpd_req_t *)&req, buffer, 1U, &received));
    TEST_ASSERT_EQUAL_UINT32(99U, received);
    TEST_ASSERT_EQUAL_UINT32(3U, req.call_count);
}

static void test_rest_api_request_recv_exact_resets_timeout_budget_after_progress(void)
{
    static const int responses[] = {
        HTTPD_SOCK_ERR_TIMEOUT,
        1,
        HTTPD_SOCK_ERR_TIMEOUT,
        HTTPD_SOCK_ERR_TIMEOUT,
        1,
    };
    httpd_req req = {
        .responses = responses,
        .response_count = sizeof(responses) / sizeof(responses[0]),
        .payload = "ok",
    };
    char buffer[3] = {0};
    size_t received = 0U;

    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          rest_api_request_recv_exact((httpd_req_t *)&req, buffer, 2U, &received));
    TEST_ASSERT_EQUAL_UINT32(2U, received);
    TEST_ASSERT_EQUAL_STRING("ok", buffer);
    TEST_ASSERT_EQUAL_UINT32(5U, req.call_count);
}

static void test_rest_api_request_recv_exact_returns_failure_for_socket_errors(void)
{
    static const int responses[] = {-1};
    httpd_req req = {
        .responses = responses,
        .response_count = sizeof(responses) / sizeof(responses[0]),
    };
    char buffer[2] = {0};

    TEST_ASSERT_EQUAL_INT(ESP_FAIL,
                          rest_api_request_recv_exact((httpd_req_t *)&req, buffer, 1U, NULL));
    TEST_ASSERT_EQUAL_UINT32(1U, req.call_count);
}

void test_rest_api_suite(void)
{
    RUN_TEST(test_rest_api_modio_sync_from_driver_maps_absent);
    RUN_TEST(test_rest_api_modio_sync_from_driver_maps_synchronized);
    RUN_TEST(test_rest_api_modio_sync_from_driver_maps_unknown_to_absent);
    RUN_TEST(test_rest_api_network_transport_to_string_maps_known_values);
    RUN_TEST(test_rest_api_network_transport_to_string_maps_unknown_to_none);
    RUN_TEST(test_rest_api_request_recv_exact_times_out_after_three_consecutive_timeouts);
    RUN_TEST(test_rest_api_request_recv_exact_resets_timeout_budget_after_progress);
    RUN_TEST(test_rest_api_request_recv_exact_returns_failure_for_socket_errors);
}

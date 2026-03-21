#include "rest_api.h"
#include "rest_api_request_recv.h"
#include "rest_api_server_config.h"
#include "rest_api_sse_lifetime.h"
#include "rest_api_wifi_config_update.h"

#include <string.h>

#include "unity.h"

typedef struct httpd_req {
    const int *responses;
    size_t response_count;
    size_t call_count;
    const char *payload;
    size_t payload_offset;
} httpd_req;

bool httpd_uri_match_wildcard(const char *reference_uri, const char *uri_to_match, size_t match_upto)
{
    (void)reference_uri;
    (void)uri_to_match;
    (void)match_upto;
    return false;
}

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

typedef enum {
    SSE_LIFETIME_EVENT_NONE = 0,
    SSE_LIFETIME_EVENT_DELETE_QUEUE,
    SSE_LIFETIME_EVENT_COMPLETE_ASYNC_REQUEST,
    SSE_LIFETIME_EVENT_DELETE_TASK,
} sse_lifetime_event_t;

typedef struct {
    rest_api_sse_client_t *client;
    TaskHandle_t current_task;
    TaskHandle_t expected_task_during_completion;
    size_t event_count;
    sse_lifetime_event_t events[8];
    size_t lock_count;
    size_t unlock_count;
    size_t delete_task_count;
    bool expect_task_visible_during_completion;
} rest_api_sse_lifetime_test_ctx_t;

static rest_api_sse_lifetime_test_ctx_t s_sse_lifetime_test_ctx;

static void rest_api_sse_lifetime_test_reset(rest_api_sse_client_t *client)
{
    memset(&s_sse_lifetime_test_ctx, 0, sizeof(s_sse_lifetime_test_ctx));
    s_sse_lifetime_test_ctx.client = client;
}

static void rest_api_sse_lifetime_record_event(sse_lifetime_event_t event)
{
    TEST_ASSERT_TRUE(s_sse_lifetime_test_ctx.event_count < (sizeof(s_sse_lifetime_test_ctx.events) /
                                                            sizeof(s_sse_lifetime_test_ctx.events[0])));
    s_sse_lifetime_test_ctx.events[s_sse_lifetime_test_ctx.event_count++] = event;
}

static bool rest_api_sse_lifetime_test_lock(void)
{
    ++s_sse_lifetime_test_ctx.lock_count;
    return true;
}

static void rest_api_sse_lifetime_test_unlock(void)
{
    ++s_sse_lifetime_test_ctx.unlock_count;
}

static void rest_api_sse_lifetime_test_delete_queue(QueueHandle_t queue)
{
    TEST_ASSERT_NOT_NULL(queue);
    rest_api_sse_lifetime_record_event(SSE_LIFETIME_EVENT_DELETE_QUEUE);
}

static void rest_api_sse_lifetime_test_complete_async_request(httpd_req_t *req)
{
    TEST_ASSERT_NOT_NULL(req);
    TEST_ASSERT_TRUE(s_sse_lifetime_test_ctx.client->active);
    if (s_sse_lifetime_test_ctx.expect_task_visible_during_completion) {
        TEST_ASSERT_EQUAL_PTR(s_sse_lifetime_test_ctx.expected_task_during_completion,
                              s_sse_lifetime_test_ctx.client->task_handle);
    }
    rest_api_sse_lifetime_record_event(SSE_LIFETIME_EVENT_COMPLETE_ASYNC_REQUEST);
}

static void rest_api_sse_lifetime_test_delete_task(TaskHandle_t task_handle)
{
    TEST_ASSERT_NOT_NULL(task_handle);
    TEST_ASSERT_TRUE(s_sse_lifetime_test_ctx.client->active);
    ++s_sse_lifetime_test_ctx.delete_task_count;
    rest_api_sse_lifetime_record_event(SSE_LIFETIME_EVENT_DELETE_TASK);
}

static TaskHandle_t rest_api_sse_lifetime_test_current_task_handle(void)
{
    return s_sse_lifetime_test_ctx.current_task;
}

static void test_rest_api_sse_reset_client_uses_invalid_sockfd_sentinel(void)
{
    rest_api_sse_client_t client;

    memset(&client, 0xA5, sizeof(client));
    rest_api_sse_reset_client(&client);

    TEST_ASSERT_FALSE(client.active);
    TEST_ASSERT_FALSE(client.close_requested);
    TEST_ASSERT_NULL(client.queue);
    TEST_ASSERT_NULL(client.task_handle);
    TEST_ASSERT_NULL(client.req);
    TEST_ASSERT_EQUAL_INT(REST_API_SSE_INVALID_SOCKFD, client.sockfd);
}

static void test_rest_api_sse_release_client_lifetime_keeps_slot_active_until_completion(void)
{
    static const rest_api_sse_lifetime_hooks_t hooks = {
        .lock = rest_api_sse_lifetime_test_lock,
        .unlock = rest_api_sse_lifetime_test_unlock,
        .delete_queue = rest_api_sse_lifetime_test_delete_queue,
        .complete_async_request = rest_api_sse_lifetime_test_complete_async_request,
    };
    rest_api_sse_client_t client = {
        .active = true,
        .queue = (QueueHandle_t)0x1,
        .req = (httpd_req_t *)0x2,
        .task_handle = (TaskHandle_t)0x3,
        .sockfd = 42,
    };

    rest_api_sse_lifetime_test_reset(&client);
    rest_api_sse_release_client_lifetime(&client, &hooks);

    TEST_ASSERT_EQUAL_UINT32(2U, s_sse_lifetime_test_ctx.event_count);
    TEST_ASSERT_EQUAL_INT(SSE_LIFETIME_EVENT_DELETE_QUEUE, s_sse_lifetime_test_ctx.events[0]);
    TEST_ASSERT_EQUAL_INT(SSE_LIFETIME_EVENT_COMPLETE_ASYNC_REQUEST, s_sse_lifetime_test_ctx.events[1]);
    TEST_ASSERT_EQUAL_UINT32(2U, s_sse_lifetime_test_ctx.lock_count);
    TEST_ASSERT_EQUAL_UINT32(2U, s_sse_lifetime_test_ctx.unlock_count);
    TEST_ASSERT_FALSE(client.active);
    TEST_ASSERT_NULL(client.queue);
    TEST_ASSERT_NULL(client.req);
    TEST_ASSERT_NULL(client.task_handle);
    TEST_ASSERT_EQUAL_INT(REST_API_SSE_INVALID_SOCKFD, client.sockfd);
}

static void test_rest_api_sse_release_client_lifetime_keeps_task_visible_until_completion(void)
{
    static const rest_api_sse_lifetime_hooks_t hooks = {
        .lock = rest_api_sse_lifetime_test_lock,
        .unlock = rest_api_sse_lifetime_test_unlock,
        .delete_queue = rest_api_sse_lifetime_test_delete_queue,
        .complete_async_request = rest_api_sse_lifetime_test_complete_async_request,
    };
    rest_api_sse_client_t client = {
        .active = true,
        .queue = (QueueHandle_t)0x101,
        .req = (httpd_req_t *)0x202,
        .task_handle = (TaskHandle_t)0x303,
        .sockfd = 43,
    };

    rest_api_sse_lifetime_test_reset(&client);
    s_sse_lifetime_test_ctx.expect_task_visible_during_completion = true;
    s_sse_lifetime_test_ctx.expected_task_during_completion = client.task_handle;
    rest_api_sse_release_client_lifetime(&client, &hooks);

    TEST_ASSERT_FALSE(client.active);
    TEST_ASSERT_NULL(client.queue);
    TEST_ASSERT_NULL(client.req);
    TEST_ASSERT_NULL(client.task_handle);
    TEST_ASSERT_EQUAL_INT(REST_API_SSE_INVALID_SOCKFD, client.sockfd);
}

static void test_rest_api_sse_release_startup_lifetime_keeps_slot_active_until_completion(void)
{
    static const rest_api_sse_lifetime_hooks_t hooks = {
        .lock = rest_api_sse_lifetime_test_lock,
        .unlock = rest_api_sse_lifetime_test_unlock,
        .delete_queue = rest_api_sse_lifetime_test_delete_queue,
        .complete_async_request = rest_api_sse_lifetime_test_complete_async_request,
    };
    rest_api_sse_client_t client = {
        .active = true,
        .queue = (QueueHandle_t)0x11,
        .req = (httpd_req_t *)0x22,
        .sockfd = 5,
    };

    rest_api_sse_lifetime_test_reset(&client);
    rest_api_sse_release_startup_client_lifetime(&client, NULL, &hooks);

    TEST_ASSERT_EQUAL_UINT32(2U, s_sse_lifetime_test_ctx.event_count);
    TEST_ASSERT_EQUAL_INT(SSE_LIFETIME_EVENT_DELETE_QUEUE, s_sse_lifetime_test_ctx.events[0]);
    TEST_ASSERT_EQUAL_INT(SSE_LIFETIME_EVENT_COMPLETE_ASYNC_REQUEST, s_sse_lifetime_test_ctx.events[1]);
    TEST_ASSERT_EQUAL_UINT32(2U, s_sse_lifetime_test_ctx.lock_count);
    TEST_ASSERT_EQUAL_UINT32(2U, s_sse_lifetime_test_ctx.unlock_count);
    TEST_ASSERT_FALSE(client.active);
    TEST_ASSERT_NULL(client.queue);
    TEST_ASSERT_NULL(client.req);
    TEST_ASSERT_EQUAL_INT(REST_API_SSE_INVALID_SOCKFD, client.sockfd);
}

static void test_rest_api_sse_release_startup_lifetime_uses_fallback_async_request(void)
{
    static const rest_api_sse_lifetime_hooks_t hooks = {
        .lock = rest_api_sse_lifetime_test_lock,
        .unlock = rest_api_sse_lifetime_test_unlock,
        .complete_async_request = rest_api_sse_lifetime_test_complete_async_request,
    };
    httpd_req_t *fallback_req = (httpd_req_t *)0x44;
    rest_api_sse_client_t client = {
        .active = true,
        .sockfd = 6,
    };

    rest_api_sse_lifetime_test_reset(&client);
    rest_api_sse_release_startup_client_lifetime(&client, fallback_req, &hooks);

    TEST_ASSERT_EQUAL_UINT32(1U, s_sse_lifetime_test_ctx.event_count);
    TEST_ASSERT_EQUAL_INT(SSE_LIFETIME_EVENT_COMPLETE_ASYNC_REQUEST, s_sse_lifetime_test_ctx.events[0]);
    TEST_ASSERT_EQUAL_UINT32(2U, s_sse_lifetime_test_ctx.lock_count);
    TEST_ASSERT_EQUAL_UINT32(2U, s_sse_lifetime_test_ctx.unlock_count);
    TEST_ASSERT_FALSE(client.active);
    TEST_ASSERT_NULL(client.queue);
    TEST_ASSERT_NULL(client.req);
    TEST_ASSERT_EQUAL_INT(REST_API_SSE_INVALID_SOCKFD, client.sockfd);
}

static void test_rest_api_sse_force_release_deletes_foreign_task_before_completion(void)
{
    static const rest_api_sse_lifetime_hooks_t hooks = {
        .lock = rest_api_sse_lifetime_test_lock,
        .unlock = rest_api_sse_lifetime_test_unlock,
        .delete_queue = rest_api_sse_lifetime_test_delete_queue,
        .complete_async_request = rest_api_sse_lifetime_test_complete_async_request,
        .delete_task = rest_api_sse_lifetime_test_delete_task,
        .current_task_handle = rest_api_sse_lifetime_test_current_task_handle,
    };
    rest_api_sse_client_t client = {
        .active = true,
        .queue = (QueueHandle_t)0x10,
        .req = (httpd_req_t *)0x20,
        .task_handle = (TaskHandle_t)0x30,
        .sockfd = 7,
    };

    rest_api_sse_lifetime_test_reset(&client);
    s_sse_lifetime_test_ctx.current_task = (TaskHandle_t)0x40;
    rest_api_sse_force_release_client_lifetime(&client, &hooks);

    TEST_ASSERT_EQUAL_UINT32(3U, s_sse_lifetime_test_ctx.event_count);
    TEST_ASSERT_EQUAL_INT(SSE_LIFETIME_EVENT_DELETE_TASK, s_sse_lifetime_test_ctx.events[0]);
    TEST_ASSERT_EQUAL_INT(SSE_LIFETIME_EVENT_DELETE_QUEUE, s_sse_lifetime_test_ctx.events[1]);
    TEST_ASSERT_EQUAL_INT(SSE_LIFETIME_EVENT_COMPLETE_ASYNC_REQUEST, s_sse_lifetime_test_ctx.events[2]);
    TEST_ASSERT_EQUAL_UINT32(1U, s_sse_lifetime_test_ctx.delete_task_count);
    TEST_ASSERT_FALSE(client.active);
    TEST_ASSERT_NULL(client.queue);
    TEST_ASSERT_NULL(client.req);
    TEST_ASSERT_NULL(client.task_handle);
    TEST_ASSERT_EQUAL_INT(REST_API_SSE_INVALID_SOCKFD, client.sockfd);
}

static void test_rest_api_sse_force_release_skips_delete_for_current_task(void)
{
    static const rest_api_sse_lifetime_hooks_t hooks = {
        .lock = rest_api_sse_lifetime_test_lock,
        .unlock = rest_api_sse_lifetime_test_unlock,
        .delete_queue = rest_api_sse_lifetime_test_delete_queue,
        .complete_async_request = rest_api_sse_lifetime_test_complete_async_request,
        .delete_task = rest_api_sse_lifetime_test_delete_task,
        .current_task_handle = rest_api_sse_lifetime_test_current_task_handle,
    };
    TaskHandle_t current_task = (TaskHandle_t)0x50;
    rest_api_sse_client_t client = {
        .active = true,
        .queue = (QueueHandle_t)0x60,
        .req = (httpd_req_t *)0x70,
        .task_handle = current_task,
        .sockfd = 8,
    };

    rest_api_sse_lifetime_test_reset(&client);
    s_sse_lifetime_test_ctx.current_task = current_task;
    rest_api_sse_force_release_client_lifetime(&client, &hooks);

    TEST_ASSERT_EQUAL_UINT32(2U, s_sse_lifetime_test_ctx.event_count);
    TEST_ASSERT_EQUAL_INT(SSE_LIFETIME_EVENT_DELETE_QUEUE, s_sse_lifetime_test_ctx.events[0]);
    TEST_ASSERT_EQUAL_INT(SSE_LIFETIME_EVENT_COMPLETE_ASYNC_REQUEST, s_sse_lifetime_test_ctx.events[1]);
    TEST_ASSERT_EQUAL_UINT32(0U, s_sse_lifetime_test_ctx.delete_task_count);
    TEST_ASSERT_FALSE(client.active);
    TEST_ASSERT_NULL(client.queue);
    TEST_ASSERT_NULL(client.req);
    TEST_ASSERT_NULL(client.task_handle);
    TEST_ASSERT_EQUAL_INT(REST_API_SSE_INVALID_SOCKFD, client.sockfd);
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

static void test_rest_api_modio_sync_from_driver_maps_unknown(void)
{
    TEST_ASSERT_EQUAL_INT(REST_API_MODIO_SYNC_UNKNOWN,
                          rest_api_modio_sync_from_driver(MOD_IO_RELAY_SYNC_UNKNOWN));
}

static void test_rest_api_modio_sync_from_driver_maps_invalid_to_unknown(void)
{
    TEST_ASSERT_EQUAL_INT(REST_API_MODIO_SYNC_UNKNOWN,
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

static void test_rest_api_make_httpd_config_sets_explicit_server_stack_size(void)
{
    httpd_config_t config = rest_api_make_httpd_config(0U, 18U);

    TEST_ASSERT_EQUAL_UINT32(REST_API_DEFAULT_PORT, config.server_port);
    TEST_ASSERT_EQUAL_UINT32(REST_API_HTTPD_STACK_SIZE, config.stack_size);
    TEST_ASSERT_TRUE(config.lru_purge_enable);
    TEST_ASSERT_EQUAL_UINT32(18U, config.max_uri_handlers);
    TEST_ASSERT_EQUAL_PTR(httpd_uri_match_wildcard, config.uri_match_fn);
}

static void test_rest_api_wifi_config_update_rejects_passphrase_with_cleared_ssid_in_any_order(void)
{
    rest_api_wifi_config_update_request_t request = {0};
    const char *error_code = NULL;
    const char *error_message = NULL;

    TEST_ASSERT_EQUAL(ESP_OK,
                      rest_api_wifi_config_update_set_passphrase(&request,
                                                                 "secret123",
                                                                 &error_code,
                                                                 &error_message));
    TEST_ASSERT_EQUAL(ESP_OK,
                      rest_api_wifi_config_update_set_ssid(&request, NULL, true, &error_code, &error_message));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      rest_api_wifi_config_update_validate(&request,
                                                           true,
                                                           &error_code,
                                                           &error_message));
    TEST_ASSERT_EQUAL_STRING("INVALID_CONFIG_VALUE", error_code);
    TEST_ASSERT_EQUAL_STRING("passphrase cannot be combined with ssid=null", error_message);

    memset(&request, 0, sizeof(request));
    error_code = NULL;
    error_message = NULL;

    TEST_ASSERT_EQUAL(ESP_OK,
                      rest_api_wifi_config_update_set_ssid(&request, NULL, true, &error_code, &error_message));
    TEST_ASSERT_EQUAL(ESP_OK,
                      rest_api_wifi_config_update_set_passphrase(&request,
                                                                 "secret123",
                                                                 &error_code,
                                                                 &error_message));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      rest_api_wifi_config_update_validate(&request,
                                                           true,
                                                           &error_code,
                                                           &error_message));
    TEST_ASSERT_EQUAL_STRING("INVALID_CONFIG_VALUE", error_code);
    TEST_ASSERT_EQUAL_STRING("passphrase cannot be combined with ssid=null", error_message);
}

void test_rest_api_suite(void)
{
    RUN_TEST(test_rest_api_sse_reset_client_uses_invalid_sockfd_sentinel);
    RUN_TEST(test_rest_api_sse_release_client_lifetime_keeps_slot_active_until_completion);
    RUN_TEST(test_rest_api_sse_release_client_lifetime_keeps_task_visible_until_completion);
    RUN_TEST(test_rest_api_sse_release_startup_lifetime_keeps_slot_active_until_completion);
    RUN_TEST(test_rest_api_sse_release_startup_lifetime_uses_fallback_async_request);
    RUN_TEST(test_rest_api_sse_force_release_deletes_foreign_task_before_completion);
    RUN_TEST(test_rest_api_sse_force_release_skips_delete_for_current_task);
    RUN_TEST(test_rest_api_modio_sync_from_driver_maps_absent);
    RUN_TEST(test_rest_api_modio_sync_from_driver_maps_synchronized);
    RUN_TEST(test_rest_api_modio_sync_from_driver_maps_unknown);
    RUN_TEST(test_rest_api_modio_sync_from_driver_maps_invalid_to_unknown);
    RUN_TEST(test_rest_api_network_transport_to_string_maps_known_values);
    RUN_TEST(test_rest_api_network_transport_to_string_maps_unknown_to_none);
    RUN_TEST(test_rest_api_request_recv_exact_times_out_after_three_consecutive_timeouts);
    RUN_TEST(test_rest_api_request_recv_exact_resets_timeout_budget_after_progress);
    RUN_TEST(test_rest_api_request_recv_exact_returns_failure_for_socket_errors);
    RUN_TEST(test_rest_api_make_httpd_config_sets_explicit_server_stack_size);
    RUN_TEST(test_rest_api_wifi_config_update_rejects_passphrase_with_cleared_ssid_in_any_order);
}

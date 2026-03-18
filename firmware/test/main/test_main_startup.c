#include "main_startup_stubs.h"

#include "unity.h"

void app_main(void);

void setUp(void)
{
    main_startup_stub_reset();
}

void tearDown(void)
{
}

static void test_assert_call_sequence(const main_startup_call_t *expected, size_t expected_count)
{
    size_t index;

    TEST_ASSERT_EQUAL_UINT32(expected_count, main_startup_stub_get_call_count());
    for (index = 0; index < expected_count; ++index) {
        TEST_ASSERT_EQUAL_INT(expected[index], main_startup_stub_get_call(index));
    }
}

static void test_app_main_initializes_network_before_starting_http(void)
{
    static const main_startup_call_t expected_calls[] = {
        MAIN_STARTUP_CALL_EVENT_LOOP_CREATE_DEFAULT,
        MAIN_STARTUP_CALL_DEVICE_CONFIG_INIT,
        MAIN_STARTUP_CALL_BOARD_INIT,
        MAIN_STARTUP_CALL_RELAY_INIT,
        MAIN_STARTUP_CALL_MOD_IO_INIT,
        MAIN_STARTUP_CALL_INPUT_MONITOR_START,
        MAIN_STARTUP_CALL_AUTH_INIT,
        MAIN_STARTUP_CALL_NETWORK_INIT,
        MAIN_STARTUP_CALL_NETWORK_WAIT_FOR_IP,
        MAIN_STARTUP_CALL_NETWORK_REGISTER_MDNS_SERVICE,
        MAIN_STARTUP_CALL_REST_API_START,
    };
    const rest_api_config_t *config;
    rest_api_status_view_t status;
    network_status_t network_status = {
        .connected = true,
        .hostname = "relay-test",
        .ip = "192.0.2.44",
        .netmask = "255.255.255.0",
        .gateway = "192.0.2.1",
    };
    mod_io_status_t mod_io_status = {
        .present = true,
        .relay_sync = MOD_IO_RELAY_SYNC_SYNCHRONIZED,
        .relay_mask = 0x00U,
    };

    main_startup_stub_set_network_status(&network_status);
    main_startup_stub_set_mod_io_status(&mod_io_status);

    app_main();

    test_assert_call_sequence(expected_calls, sizeof(expected_calls) / sizeof(expected_calls[0]));
    TEST_ASSERT_EQUAL_UINT32(NETWORK_DEFAULT_WAIT_FOR_IP_TIMEOUT_MS,
                             main_startup_stub_get_last_wait_timeout_ms());
    TEST_ASSERT_EQUAL_UINT16(REST_API_DEFAULT_PORT, main_startup_stub_get_last_mdns_port());

    config = main_startup_stub_get_last_rest_api_config();
    TEST_ASSERT_NOT_NULL(config);
    TEST_ASSERT_EQUAL_UINT16(REST_API_DEFAULT_PORT, config->port);
    TEST_ASSERT_NOT_NULL(config->auth_handler);
    TEST_ASSERT_NOT_NULL(config->status_provider);
    TEST_ASSERT_EQUAL(REST_API_AUTH_RESULT_ALLOW, config->auth_handler(NULL, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, config->status_provider(&status, config->status_ctx));
    TEST_ASSERT_TRUE(status.network.connected);
    TEST_ASSERT_EQUAL_STRING("relay-test", status.network.hostname);
    TEST_ASSERT_EQUAL_STRING("192.0.2.44", status.network.ip);
    TEST_ASSERT_EQUAL_STRING("255.255.255.0", status.network.netmask);
    TEST_ASSERT_EQUAL_STRING("192.0.2.1", status.network.gateway);
    TEST_ASSERT_TRUE(status.modio_present);
    TEST_ASSERT_EQUAL_INT(REST_API_MODIO_SYNC_SYNCHRONIZED, status.modio_sync);
}

static void test_app_main_stops_before_http_when_network_init_fails(void)
{
    static const main_startup_call_t expected_calls[] = {
        MAIN_STARTUP_CALL_EVENT_LOOP_CREATE_DEFAULT,
        MAIN_STARTUP_CALL_DEVICE_CONFIG_INIT,
        MAIN_STARTUP_CALL_BOARD_INIT,
        MAIN_STARTUP_CALL_RELAY_INIT,
        MAIN_STARTUP_CALL_MOD_IO_INIT,
        MAIN_STARTUP_CALL_INPUT_MONITOR_START,
        MAIN_STARTUP_CALL_AUTH_INIT,
        MAIN_STARTUP_CALL_NETWORK_INIT,
    };

    main_startup_stub_set_network_init_result(ESP_FAIL);

    app_main();

    test_assert_call_sequence(expected_calls, sizeof(expected_calls) / sizeof(expected_calls[0]));
    TEST_ASSERT_NULL(main_startup_stub_get_last_rest_api_config());
}

static void test_app_main_stops_before_network_when_input_monitor_fails(void)
{
    static const main_startup_call_t expected_calls[] = {
        MAIN_STARTUP_CALL_EVENT_LOOP_CREATE_DEFAULT,
        MAIN_STARTUP_CALL_DEVICE_CONFIG_INIT,
        MAIN_STARTUP_CALL_BOARD_INIT,
        MAIN_STARTUP_CALL_RELAY_INIT,
        MAIN_STARTUP_CALL_MOD_IO_INIT,
        MAIN_STARTUP_CALL_INPUT_MONITOR_START,
    };

    main_startup_stub_set_input_monitor_start_result(ESP_FAIL);

    app_main();

    test_assert_call_sequence(expected_calls, sizeof(expected_calls) / sizeof(expected_calls[0]));
    TEST_ASSERT_NULL(main_startup_stub_get_last_rest_api_config());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_app_main_initializes_network_before_starting_http);
    RUN_TEST(test_app_main_stops_before_http_when_network_init_fails);
    RUN_TEST(test_app_main_stops_before_network_when_input_monitor_fails);
    return UNITY_END();
}

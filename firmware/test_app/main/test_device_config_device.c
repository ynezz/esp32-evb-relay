#include <stdbool.h>
#include <string.h>

#include "device_config.h"
#include "nvs.h"
#include "unity.h"

#define DEVICE_CONFIG_TEST_NAMESPACE "device_cfg"

typedef struct {
    device_config_snapshot_t snapshot;
    char api_token[DEVICE_CONFIG_API_TOKEN_MAX_LEN + 1];
    bool api_token_set;
} device_config_fixture_t;

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

    device_config_reset_for_testing();
}

static void erase_device_config_namespace(void)
{
    nvs_handle_t handle = 0;

    TEST_ASSERT_EQUAL(ESP_OK, nvs_open(DEVICE_CONFIG_TEST_NAMESPACE, NVS_READWRITE, &handle));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_erase_all(handle));
    TEST_ASSERT_EQUAL(ESP_OK, nvs_commit(handle));
    nvs_close(handle);

    device_config_reset_for_testing();
}

static void assert_device_config_values(const char *expected_token,
                                        bool expected_token_set,
                                        uint32_t expected_poll_interval_ms,
                                        const char *expected_hostname,
                                        device_config_modio_boot_policy_t expected_policy)
{
    char api_token[DEVICE_CONFIG_API_TOKEN_MAX_LEN + 1];
    bool api_token_set = false;
    uint32_t poll_interval_ms = 0;
    char hostname[DEVICE_CONFIG_HOSTNAME_MAX_LEN + 1];
    device_config_modio_boot_policy_t policy = DEVICE_CONFIG_MODIO_BOOT_POLICY_LEAVE_UNCHANGED;
    device_config_snapshot_t snapshot;

    TEST_ASSERT_EQUAL(ESP_OK, device_config_get_snapshot(&snapshot));
    TEST_ASSERT_EQUAL_UINT32(expected_poll_interval_ms, snapshot.poll_interval_ms);
    TEST_ASSERT_EQUAL_STRING(expected_hostname, snapshot.hostname);
    TEST_ASSERT_EQUAL_INT(expected_policy, snapshot.modio_boot_policy);
    TEST_ASSERT_EQUAL(expected_token_set, snapshot.api_token_set);

    TEST_ASSERT_EQUAL(ESP_OK, device_config_get_api_token(api_token, sizeof(api_token), &api_token_set));
    TEST_ASSERT_EQUAL(expected_token_set, api_token_set);
    TEST_ASSERT_EQUAL_STRING(expected_token, api_token);

    TEST_ASSERT_EQUAL(ESP_OK, device_config_get_poll_interval_ms(&poll_interval_ms));
    TEST_ASSERT_EQUAL_UINT32(expected_poll_interval_ms, poll_interval_ms);

    TEST_ASSERT_EQUAL(ESP_OK, device_config_get_hostname(hostname, sizeof(hostname)));
    TEST_ASSERT_EQUAL_STRING(expected_hostname, hostname);

    TEST_ASSERT_EQUAL(ESP_OK, device_config_get_modio_boot_policy(&policy));
    TEST_ASSERT_EQUAL_INT(expected_policy, policy);
}

TEST_CASE("device_config device loads defaults from a fresh namespace",
          "[qa][device_config][device]")
{
    device_config_fixture_t fixture = {0};
    volatile bool fixture_captured = false;

    if (TEST_PROTECT()) {
        capture_device_config_fixture(&fixture);
        fixture_captured = true;

        erase_device_config_namespace();

        assert_device_config_values("",
                                    false,
                                    DEVICE_CONFIG_DEFAULT_POLL_INTERVAL_MS,
                                    DEVICE_CONFIG_DEFAULT_HOSTNAME,
                                    DEVICE_CONFIG_MODIO_BOOT_POLICY_LEAVE_UNCHANGED);
    }

    if (fixture_captured) {
        restore_device_config_fixture(&fixture);
    }
}

TEST_CASE("device_config device round-trips every key through the public API",
          "[qa][device_config][device]")
{
    device_config_fixture_t fixture = {0};
    volatile bool fixture_captured = false;

    if (TEST_PROTECT()) {
        capture_device_config_fixture(&fixture);
        fixture_captured = true;

        TEST_ASSERT_EQUAL(ESP_OK, device_config_set_api_token("device-test-token", NULL));
        TEST_ASSERT_EQUAL(ESP_OK, device_config_set_poll_interval_ms(275U, NULL));
        TEST_ASSERT_EQUAL(ESP_OK, device_config_set_hostname("device-config-lab", NULL));
        TEST_ASSERT_EQUAL(ESP_OK,
                          device_config_set_modio_boot_policy(DEVICE_CONFIG_MODIO_BOOT_POLICY_ALL_OFF,
                                                              NULL));

        assert_device_config_values("device-test-token",
                                    true,
                                    275U,
                                    "device-config-lab",
                                    DEVICE_CONFIG_MODIO_BOOT_POLICY_ALL_OFF);
    }

    if (fixture_captured) {
        restore_device_config_fixture(&fixture);
    }
}

TEST_CASE("device_config device persists values across re-init",
          "[qa][device_config][device]")
{
    device_config_fixture_t fixture = {0};
    volatile bool fixture_captured = false;

    if (TEST_PROTECT()) {
        capture_device_config_fixture(&fixture);
        fixture_captured = true;

        TEST_ASSERT_EQUAL(ESP_OK, device_config_set_api_token("persisted-device-token", NULL));
        TEST_ASSERT_EQUAL(ESP_OK, device_config_set_poll_interval_ms(640U, NULL));
        TEST_ASSERT_EQUAL(ESP_OK, device_config_set_hostname("device-config-persist", NULL));
        TEST_ASSERT_EQUAL(ESP_OK,
                          device_config_set_modio_boot_policy(DEVICE_CONFIG_MODIO_BOOT_POLICY_ALL_OFF,
                                                              NULL));

        device_config_reset_for_testing();

        assert_device_config_values("persisted-device-token",
                                    true,
                                    640U,
                                    "device-config-persist",
                                    DEVICE_CONFIG_MODIO_BOOT_POLICY_ALL_OFF);
    }

    if (fixture_captured) {
        restore_device_config_fixture(&fixture);
    }
}

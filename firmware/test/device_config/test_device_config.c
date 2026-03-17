#include <string.h>

#include "device_config.h"
#include "unity.h"

static void fill_repeated_string(char *buffer, size_t buffer_size, char value)
{
    memset(buffer, value, buffer_size - 1U);
    buffer[buffer_size - 1U] = '\0';
}

static void assert_hostname_accepts(const char *hostname)
{
    char actual[DEVICE_CONFIG_HOSTNAME_MAX_LEN + 1];
    device_config_apply_result_t result = {0};

    TEST_ASSERT_EQUAL(ESP_OK, device_config_set_hostname(hostname, &result));
    TEST_ASSERT_FALSE(result.live);
    TEST_ASSERT_TRUE(result.restart_required);
    TEST_ASSERT_EQUAL(ESP_OK, device_config_get_hostname(actual, sizeof(actual)));
    TEST_ASSERT_EQUAL_STRING(hostname, actual);
}

static void assert_hostname_rejects(const char *hostname)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, device_config_set_hostname(hostname, NULL));
}

static void test_device_config_defaults(void)
{
    device_config_snapshot_t snapshot;

    TEST_ASSERT_EQUAL(ESP_OK, device_config_get_snapshot(&snapshot));
    TEST_ASSERT_EQUAL_UINT32(DEVICE_CONFIG_DEFAULT_POLL_INTERVAL_MS, snapshot.poll_interval_ms);
    TEST_ASSERT_EQUAL_STRING(DEVICE_CONFIG_DEFAULT_HOSTNAME, snapshot.hostname);
    TEST_ASSERT_EQUAL_INT(DEVICE_CONFIG_MODIO_BOOT_POLICY_LEAVE_UNCHANGED,
                          snapshot.modio_boot_policy);
    TEST_ASSERT_FALSE(snapshot.api_token_set);
}

static void test_device_config_poll_interval_validation(void)
{
    uint32_t poll_interval_ms = 0;
    device_config_apply_result_t result = {0};

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, device_config_set_poll_interval_ms(49U, NULL));

    TEST_ASSERT_EQUAL(ESP_OK, device_config_set_poll_interval_ms(50U, &result));
    TEST_ASSERT_TRUE(result.live);
    TEST_ASSERT_FALSE(result.restart_required);
    TEST_ASSERT_EQUAL(ESP_OK, device_config_get_poll_interval_ms(&poll_interval_ms));
    TEST_ASSERT_EQUAL_UINT32(50U, poll_interval_ms);

    TEST_ASSERT_EQUAL(ESP_OK, device_config_set_poll_interval_ms(10000U, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, device_config_get_poll_interval_ms(&poll_interval_ms));
    TEST_ASSERT_EQUAL_UINT32(10000U, poll_interval_ms);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, device_config_set_poll_interval_ms(10001U, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, device_config_get_poll_interval_ms(&poll_interval_ms));
    TEST_ASSERT_EQUAL_UINT32(10000U, poll_interval_ms);
}

static void test_device_config_hostname_validation(void)
{
    char hostname_63[DEVICE_CONFIG_HOSTNAME_MAX_LEN + 1];
    char hostname_64[DEVICE_CONFIG_HOSTNAME_MAX_LEN + 2];

    fill_repeated_string(hostname_63, sizeof(hostname_63), 'a');
    fill_repeated_string(hostname_64, sizeof(hostname_64), 'a');

    assert_hostname_rejects("");
    assert_hostname_accepts("a");
    assert_hostname_accepts(hostname_63);
    assert_hostname_rejects(hostname_64);
    assert_hostname_rejects("-relay");
    assert_hostname_rejects("relay-");
    assert_hostname_accepts("relay-01");
    assert_hostname_rejects("host.name");
    assert_hostname_rejects("host_name");
    assert_hostname_accepts("relay01");
}

static void test_device_config_modio_boot_policy_parse_and_round_trip(void)
{
    device_config_modio_boot_policy_t policy = DEVICE_CONFIG_MODIO_BOOT_POLICY_ALL_OFF;

    TEST_ASSERT_EQUAL(ESP_OK,
                      device_config_parse_modio_boot_policy("leave_unchanged", &policy));
    TEST_ASSERT_EQUAL_INT(DEVICE_CONFIG_MODIO_BOOT_POLICY_LEAVE_UNCHANGED, policy);
    TEST_ASSERT_EQUAL_STRING("leave_unchanged",
                             device_config_modio_boot_policy_to_string(policy));

    TEST_ASSERT_EQUAL(ESP_OK, device_config_parse_modio_boot_policy("all_off", &policy));
    TEST_ASSERT_EQUAL_INT(DEVICE_CONFIG_MODIO_BOOT_POLICY_ALL_OFF, policy);
    TEST_ASSERT_EQUAL_STRING("all_off", device_config_modio_boot_policy_to_string(policy));

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, device_config_parse_modio_boot_policy("bogus", &policy));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, device_config_parse_modio_boot_policy(NULL, &policy));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      device_config_parse_modio_boot_policy("all_off", NULL));
}

static void test_device_config_api_token_validation(void)
{
    char actual[DEVICE_CONFIG_API_TOKEN_MAX_LEN + 1];
    char max_token[DEVICE_CONFIG_API_TOKEN_MAX_LEN + 1];
    char too_long_token[DEVICE_CONFIG_API_TOKEN_MAX_LEN + 2];
    bool is_set = false;
    device_config_snapshot_t snapshot;
    device_config_apply_result_t result = {0};

    fill_repeated_string(max_token, sizeof(max_token), 't');
    fill_repeated_string(too_long_token, sizeof(too_long_token), 't');

    TEST_ASSERT_EQUAL(ESP_OK, device_config_set_api_token(NULL, &result));
    TEST_ASSERT_TRUE(result.live);
    TEST_ASSERT_FALSE(result.restart_required);
    TEST_ASSERT_EQUAL(ESP_OK, device_config_set_api_token("", NULL));

    TEST_ASSERT_EQUAL(ESP_OK, device_config_set_api_token(max_token, &result));
    TEST_ASSERT_TRUE(result.live);
    TEST_ASSERT_FALSE(result.restart_required);
    TEST_ASSERT_EQUAL(ESP_OK, device_config_get_api_token(actual, sizeof(actual), &is_set));
    TEST_ASSERT_TRUE(is_set);
    TEST_ASSERT_EQUAL_STRING(max_token, actual);

    TEST_ASSERT_EQUAL(ESP_OK, device_config_set_api_token("", NULL));
    TEST_ASSERT_EQUAL(ESP_OK, device_config_get_api_token(actual, sizeof(actual), &is_set));
    TEST_ASSERT_FALSE(is_set);
    TEST_ASSERT_EQUAL_STRING("", actual);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, device_config_set_api_token(too_long_token, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, device_config_get_snapshot(&snapshot));
    TEST_ASSERT_FALSE(snapshot.api_token_set);
}

static void test_device_config_snapshot_persists_values(void)
{
    char actual_token[DEVICE_CONFIG_API_TOKEN_MAX_LEN + 1];
    bool is_set = false;
    device_config_snapshot_t snapshot;

    TEST_ASSERT_EQUAL(ESP_OK, device_config_set_api_token("secret-token", NULL));
    TEST_ASSERT_EQUAL(ESP_OK, device_config_set_poll_interval_ms(250U, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, device_config_set_hostname("factory-relay", NULL));
    TEST_ASSERT_EQUAL(ESP_OK,
                      device_config_set_modio_boot_policy(DEVICE_CONFIG_MODIO_BOOT_POLICY_ALL_OFF,
                                                          NULL));

    device_config_reset_for_testing();

    TEST_ASSERT_EQUAL(ESP_OK, device_config_get_snapshot(&snapshot));
    TEST_ASSERT_EQUAL_UINT32(250U, snapshot.poll_interval_ms);
    TEST_ASSERT_EQUAL_STRING("factory-relay", snapshot.hostname);
    TEST_ASSERT_EQUAL_INT(DEVICE_CONFIG_MODIO_BOOT_POLICY_ALL_OFF, snapshot.modio_boot_policy);
    TEST_ASSERT_TRUE(snapshot.api_token_set);

    TEST_ASSERT_EQUAL(ESP_OK,
                      device_config_get_api_token(actual_token, sizeof(actual_token), &is_set));
    TEST_ASSERT_TRUE(is_set);
    TEST_ASSERT_EQUAL_STRING("secret-token", actual_token);
}

static void test_device_config_key_descriptors(void)
{
    typedef struct {
        device_config_key_t key;
        const char *name;
        bool secret;
        bool live;
        bool restart_required;
    } expected_key_descriptor_t;

    static const expected_key_descriptor_t expected[] = {
        {DEVICE_CONFIG_KEY_API_TOKEN, "api_token", true, true, false},
        {DEVICE_CONFIG_KEY_POLL_INTERVAL_MS, "poll_interval_ms", false, true, false},
        {DEVICE_CONFIG_KEY_HOSTNAME, "hostname", false, false, true},
        {DEVICE_CONFIG_KEY_MODIO_BOOT_POLICY, "modio_boot_policy", false, false, false},
    };

    for (size_t i = 0; i < (sizeof(expected) / sizeof(expected[0])); ++i) {
        const device_config_key_descriptor_t *descriptor =
            device_config_get_key_descriptor(expected[i].key);

        TEST_ASSERT_NOT_NULL(descriptor);
        TEST_ASSERT_EQUAL_INT(expected[i].key, descriptor->key);
        TEST_ASSERT_EQUAL_STRING(expected[i].name, descriptor->name);
        TEST_ASSERT_EQUAL(expected[i].secret, descriptor->secret);
        TEST_ASSERT_EQUAL(expected[i].live, descriptor->live);
        TEST_ASSERT_EQUAL(expected[i].restart_required, descriptor->restart_required);
    }

    TEST_ASSERT_NULL(device_config_get_key_descriptor((device_config_key_t)-1));
    TEST_ASSERT_NULL(device_config_get_key_descriptor((device_config_key_t)DEVICE_CONFIG_KEY_COUNT));
}

void test_device_config_suite(void)
{
    RUN_TEST(test_device_config_defaults);
    RUN_TEST(test_device_config_poll_interval_validation);
    RUN_TEST(test_device_config_hostname_validation);
    RUN_TEST(test_device_config_modio_boot_policy_parse_and_round_trip);
    RUN_TEST(test_device_config_api_token_validation);
    RUN_TEST(test_device_config_snapshot_persists_values);
    RUN_TEST(test_device_config_key_descriptors);
}

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
    TEST_ASSERT_EQUAL_INT(DEVICE_CONFIG_APPLY_MODE_RESTART_REQUIRED, result.apply_mode);
    TEST_ASSERT_EQUAL(ESP_OK, device_config_get_hostname(actual, sizeof(actual)));
    TEST_ASSERT_EQUAL_STRING(hostname, actual);
}

static void assert_hostname_rejects(const char *hostname)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, device_config_set_hostname(hostname, NULL));
}

static void assert_wifi_credentials(const char *ssid, const char *passphrase)
{
    device_config_wifi_sta_credentials_t credentials;

    TEST_ASSERT_EQUAL(ESP_OK, device_config_get_wifi_sta_credentials(&credentials));
    if ((ssid == NULL) || (ssid[0] == '\0')) {
        TEST_ASSERT_FALSE(credentials.ssid_set);
        TEST_ASSERT_FALSE(credentials.passphrase_set);
        TEST_ASSERT_EQUAL_STRING("", credentials.ssid);
        TEST_ASSERT_EQUAL_STRING("", credentials.passphrase);
        return;
    }

    TEST_ASSERT_TRUE(credentials.ssid_set);
    TEST_ASSERT_EQUAL_STRING(ssid, credentials.ssid);
    TEST_ASSERT_EQUAL_STRING(passphrase, credentials.passphrase);
    TEST_ASSERT_EQUAL((passphrase != NULL) && (passphrase[0] != '\0'), credentials.passphrase_set);
}

static void test_device_config_defaults(void)
{
    device_config_snapshot_t snapshot;

    TEST_ASSERT_EQUAL(ESP_OK, device_config_get_snapshot(&snapshot));
    TEST_ASSERT_EQUAL_UINT32(DEVICE_CONFIG_DEFAULT_POLL_INTERVAL_MS, snapshot.poll_interval_ms);
    TEST_ASSERT_EQUAL_STRING(DEVICE_CONFIG_DEFAULT_HOSTNAME, snapshot.hostname);
    TEST_ASSERT_FALSE(snapshot.wifi_ssid_set);
    TEST_ASSERT_FALSE(snapshot.wifi_passphrase_set);
    TEST_ASSERT_EQUAL_INT(DEVICE_CONFIG_NETWORK_POLICY_ETHERNET_ONLY, snapshot.network_policy);
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
    TEST_ASSERT_EQUAL_INT(DEVICE_CONFIG_APPLY_MODE_IMMEDIATE, result.apply_mode);
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

static void test_device_config_network_policy_parse_and_round_trip(void)
{
    device_config_network_policy_t policy = DEVICE_CONFIG_NETWORK_POLICY_PREFER_ETHERNET;

    TEST_ASSERT_EQUAL(ESP_OK,
                      device_config_parse_network_policy("ethernet_only", &policy));
    TEST_ASSERT_EQUAL_INT(DEVICE_CONFIG_NETWORK_POLICY_ETHERNET_ONLY, policy);
    TEST_ASSERT_EQUAL_STRING("ethernet_only", device_config_network_policy_to_string(policy));

    TEST_ASSERT_EQUAL(ESP_OK, device_config_parse_network_policy("wifi_only", &policy));
    TEST_ASSERT_EQUAL_INT(DEVICE_CONFIG_NETWORK_POLICY_WIFI_ONLY, policy);
    TEST_ASSERT_EQUAL_STRING("wifi_only", device_config_network_policy_to_string(policy));

    TEST_ASSERT_EQUAL(ESP_OK, device_config_parse_network_policy("prefer_ethernet", &policy));
    TEST_ASSERT_EQUAL_INT(DEVICE_CONFIG_NETWORK_POLICY_PREFER_ETHERNET, policy);
    TEST_ASSERT_EQUAL_STRING("prefer_ethernet", device_config_network_policy_to_string(policy));

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, device_config_parse_network_policy("bogus", &policy));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, device_config_parse_network_policy(NULL, &policy));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      device_config_parse_network_policy("wifi_only", NULL));
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
    TEST_ASSERT_EQUAL_INT(DEVICE_CONFIG_APPLY_MODE_IMMEDIATE, result.apply_mode);
    TEST_ASSERT_EQUAL(ESP_OK, device_config_set_api_token("", NULL));

    TEST_ASSERT_EQUAL(ESP_OK, device_config_set_api_token(max_token, &result));
    TEST_ASSERT_EQUAL_INT(DEVICE_CONFIG_APPLY_MODE_IMMEDIATE, result.apply_mode);
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

static void test_device_config_wifi_sta_credentials_validation(void)
{
    char max_ssid[DEVICE_CONFIG_WIFI_SSID_MAX_LEN + 1];
    char too_long_ssid[DEVICE_CONFIG_WIFI_SSID_MAX_LEN + 2];
    char max_passphrase[DEVICE_CONFIG_WIFI_PASSPHRASE_MAX_LEN + 1];
    char too_long_passphrase[DEVICE_CONFIG_WIFI_PASSPHRASE_MAX_LEN + 2];
    device_config_apply_result_t result = {0};

    fill_repeated_string(max_ssid, sizeof(max_ssid), 's');
    fill_repeated_string(too_long_ssid, sizeof(too_long_ssid), 's');
    fill_repeated_string(max_passphrase, sizeof(max_passphrase), 'p');
    fill_repeated_string(too_long_passphrase, sizeof(too_long_passphrase), 'p');

    TEST_ASSERT_EQUAL(ESP_OK,
                      device_config_set_wifi_sta_credentials("lab-net", "password1", &result));
    TEST_ASSERT_EQUAL_INT(DEVICE_CONFIG_APPLY_MODE_RESTART_REQUIRED, result.apply_mode);
    assert_wifi_credentials("lab-net", "password1");

    TEST_ASSERT_EQUAL(ESP_OK,
                      device_config_set_wifi_sta_credentials("guest-net", "", &result));
    TEST_ASSERT_EQUAL_INT(DEVICE_CONFIG_APPLY_MODE_RESTART_REQUIRED, result.apply_mode);
    assert_wifi_credentials("guest-net", "");

    TEST_ASSERT_EQUAL(ESP_OK,
                      device_config_set_wifi_sta_credentials(max_ssid, max_passphrase, NULL));
    assert_wifi_credentials(max_ssid, max_passphrase);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      device_config_set_wifi_sta_credentials("", "password1", NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      device_config_set_wifi_sta_credentials(NULL, "password1", NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      device_config_set_wifi_sta_credentials("lab-net", NULL, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      device_config_set_wifi_sta_credentials("lab-net", "short", NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      device_config_set_wifi_sta_credentials(too_long_ssid, max_passphrase, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      device_config_set_wifi_sta_credentials("lab-net", too_long_passphrase, NULL));

    TEST_ASSERT_EQUAL(ESP_OK, device_config_set_wifi_sta_credentials(NULL, NULL, &result));
    TEST_ASSERT_EQUAL_INT(DEVICE_CONFIG_APPLY_MODE_RESTART_REQUIRED, result.apply_mode);
    assert_wifi_credentials(NULL, NULL);
}

static void test_device_config_apply_mode_metadata(void)
{
    device_config_apply_result_t result = {0};

    TEST_ASSERT_EQUAL(ESP_OK,
                      device_config_set_wifi_sta_credentials("ops-net", "password1", &result));
    TEST_ASSERT_EQUAL_INT(DEVICE_CONFIG_APPLY_MODE_RESTART_REQUIRED, result.apply_mode);

    TEST_ASSERT_EQUAL(ESP_OK,
                      device_config_set_network_policy(DEVICE_CONFIG_NETWORK_POLICY_WIFI_ONLY,
                                                       &result));
    TEST_ASSERT_EQUAL_INT(DEVICE_CONFIG_APPLY_MODE_RESTART_REQUIRED, result.apply_mode);

    TEST_ASSERT_EQUAL(ESP_OK,
                      device_config_set_modio_boot_policy(DEVICE_CONFIG_MODIO_BOOT_POLICY_ALL_OFF,
                                                          &result));
    TEST_ASSERT_EQUAL_INT(DEVICE_CONFIG_APPLY_MODE_NEXT_BOOT, result.apply_mode);
    TEST_ASSERT_EQUAL_STRING("next_boot", device_config_apply_mode_to_string(result.apply_mode));

    TEST_ASSERT_EQUAL(ESP_OK,
                      device_config_set_hostname("metadata-host", &result));
    TEST_ASSERT_EQUAL_INT(DEVICE_CONFIG_APPLY_MODE_RESTART_REQUIRED, result.apply_mode);
    TEST_ASSERT_EQUAL_STRING("restart_required",
                             device_config_apply_mode_to_string(result.apply_mode));

    TEST_ASSERT_EQUAL_STRING("immediate",
                             device_config_apply_mode_to_string(DEVICE_CONFIG_APPLY_MODE_IMMEDIATE));
    TEST_ASSERT_EQUAL_STRING("unknown",
                             device_config_apply_mode_to_string((device_config_apply_mode_t)99));
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
                      device_config_set_wifi_sta_credentials("factory-net", "factory-pass", NULL));
    TEST_ASSERT_EQUAL(ESP_OK,
                      device_config_set_network_policy(DEVICE_CONFIG_NETWORK_POLICY_PREFER_ETHERNET,
                                                       NULL));
    TEST_ASSERT_EQUAL(ESP_OK,
                      device_config_set_modio_boot_policy(DEVICE_CONFIG_MODIO_BOOT_POLICY_ALL_OFF,
                                                          NULL));

    device_config_reset_for_testing();

    TEST_ASSERT_EQUAL(ESP_OK, device_config_get_snapshot(&snapshot));
    TEST_ASSERT_EQUAL_UINT32(250U, snapshot.poll_interval_ms);
    TEST_ASSERT_EQUAL_STRING("factory-relay", snapshot.hostname);
    TEST_ASSERT_TRUE(snapshot.wifi_ssid_set);
    TEST_ASSERT_TRUE(snapshot.wifi_passphrase_set);
    TEST_ASSERT_EQUAL_INT(DEVICE_CONFIG_NETWORK_POLICY_PREFER_ETHERNET, snapshot.network_policy);
    TEST_ASSERT_EQUAL_INT(DEVICE_CONFIG_MODIO_BOOT_POLICY_ALL_OFF, snapshot.modio_boot_policy);
    TEST_ASSERT_TRUE(snapshot.api_token_set);

    assert_wifi_credentials("factory-net", "factory-pass");
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
        device_config_apply_mode_t apply_mode;
    } expected_key_descriptor_t;

    static const expected_key_descriptor_t expected[] = {
        {DEVICE_CONFIG_KEY_API_TOKEN, "api_token", true, DEVICE_CONFIG_APPLY_MODE_IMMEDIATE},
        {DEVICE_CONFIG_KEY_POLL_INTERVAL_MS, "poll_interval_ms", false, DEVICE_CONFIG_APPLY_MODE_IMMEDIATE},
        {DEVICE_CONFIG_KEY_HOSTNAME, "hostname", false, DEVICE_CONFIG_APPLY_MODE_RESTART_REQUIRED},
        {DEVICE_CONFIG_KEY_WIFI_SSID, "wifi_ssid", true, DEVICE_CONFIG_APPLY_MODE_RESTART_REQUIRED},
        {DEVICE_CONFIG_KEY_WIFI_PASSPHRASE, "wifi_passphrase", true, DEVICE_CONFIG_APPLY_MODE_RESTART_REQUIRED},
        {DEVICE_CONFIG_KEY_NETWORK_POLICY, "network_policy", false, DEVICE_CONFIG_APPLY_MODE_RESTART_REQUIRED},
        {DEVICE_CONFIG_KEY_MODIO_BOOT_POLICY, "modio_boot_policy", false, DEVICE_CONFIG_APPLY_MODE_NEXT_BOOT},
    };

    for (size_t i = 0; i < (sizeof(expected) / sizeof(expected[0])); ++i) {
        const device_config_key_descriptor_t *descriptor =
            device_config_get_key_descriptor(expected[i].key);

        TEST_ASSERT_NOT_NULL(descriptor);
        TEST_ASSERT_EQUAL_INT(expected[i].key, descriptor->key);
        TEST_ASSERT_EQUAL_STRING(expected[i].name, descriptor->name);
        TEST_ASSERT_EQUAL(expected[i].secret, descriptor->secret);
        TEST_ASSERT_EQUAL_INT(expected[i].apply_mode, descriptor->apply_mode);
    }

    TEST_ASSERT_NULL(device_config_get_key_descriptor((device_config_key_t) -1));
    TEST_ASSERT_NULL(device_config_get_key_descriptor((device_config_key_t)DEVICE_CONFIG_KEY_COUNT));
}

void test_device_config_suite(void)
{
    RUN_TEST(test_device_config_defaults);
    RUN_TEST(test_device_config_poll_interval_validation);
    RUN_TEST(test_device_config_hostname_validation);
    RUN_TEST(test_device_config_modio_boot_policy_parse_and_round_trip);
    RUN_TEST(test_device_config_network_policy_parse_and_round_trip);
    RUN_TEST(test_device_config_api_token_validation);
    RUN_TEST(test_device_config_wifi_sta_credentials_validation);
    RUN_TEST(test_device_config_apply_mode_metadata);
    RUN_TEST(test_device_config_snapshot_persists_values);
    RUN_TEST(test_device_config_key_descriptors);
}

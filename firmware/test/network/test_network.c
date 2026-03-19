#include "network.h"

#include "esp_event_stubs.h"
#include "network_test_stubs.h"
#include "unity.h"

void setUp(void)
{
    network_test_stubs_reset();
}

void tearDown(void)
{
}

static void test_network_init_unregisters_event_handlers_after_failed_ethernet_start(void)
{
    network_test_stubs_set_network_policy(DEVICE_CONFIG_NETWORK_POLICY_ETHERNET_ONLY);
    network_test_stubs_set_eth_mac_new_failure(true);

    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, network_init());
    TEST_ASSERT_EQUAL_UINT32(4U, esp_event_stub_get_handler_register_count());
    TEST_ASSERT_EQUAL_UINT32(4U, esp_event_stub_get_handler_unregister_count());
    TEST_ASSERT_EQUAL_UINT32(0U, esp_event_stub_get_active_handler_count());

    network_test_stubs_set_eth_mac_new_failure(false);

    TEST_ASSERT_EQUAL(ESP_OK, network_init());
    TEST_ASSERT_EQUAL_UINT32(8U, esp_event_stub_get_handler_register_count());
    TEST_ASSERT_EQUAL_UINT32(4U, esp_event_stub_get_handler_unregister_count());
    TEST_ASSERT_EQUAL_UINT32(4U, esp_event_stub_get_active_handler_count());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_network_init_unregisters_event_handlers_after_failed_ethernet_start);
    return UNITY_END();
}

#include "device_config.h"
#include "gpio_stubs.h"
#include "nvs_stubs.h"
#include "relay.h"
#include "unity.h"

void test_device_config_suite(void);
void test_relay_suite(void);

void setUp(void)
{
    gpio_stub_reset();
    nvs_stub_reset();
    device_config_reset_for_testing();
    relay_reset_for_testing();
}

void tearDown(void)
{
}

int main(void)
{
    UNITY_BEGIN();
    test_device_config_suite();
    test_relay_suite();
    return UNITY_END();
}

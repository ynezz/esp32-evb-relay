#include "device_config.h"
#include "nvs_stubs.h"
#include "unity.h"

void test_device_config_suite(void);

void setUp(void)
{
    nvs_stub_reset();
    device_config_reset_for_testing();
}

void tearDown(void)
{
}

int main(void)
{
    UNITY_BEGIN();
    test_device_config_suite();
    return UNITY_END();
}

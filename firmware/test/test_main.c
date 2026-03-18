#include "device_config.h"
#include "esp_event_stubs.h"
#include "esp_idf_stubs.h"
#include "freertos_stubs.h"
#include "gpio_stubs.h"
#include "i2c_stubs.h"
#include "input_monitor.h"
#include "mod_io.h"
#include "nvs_stubs.h"
#include "relay.h"
#include "unity.h"

void test_device_config_suite(void);
void test_input_monitor_suite(void);
void test_mod_io_suite(void);
void test_relay_suite(void);

void setUp(void)
{
    gpio_stub_reset();
    i2c_stub_reset();
    nvs_stub_reset();
    esp_event_stub_reset();
    freertos_stub_reset();
    esp_stub_reset_time_override();
    device_config_reset_for_testing();
    input_monitor_reset_for_testing();
    mod_io_reset_for_testing();
    relay_reset_for_testing();
}

void tearDown(void)
{
}

int main(void)
{
    UNITY_BEGIN();
    test_device_config_suite();
    test_input_monitor_suite();
    test_mod_io_suite();
    test_relay_suite();
    return UNITY_END();
}

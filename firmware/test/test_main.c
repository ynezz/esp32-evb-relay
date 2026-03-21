#include "device_config.h"
#include "auth.h"
#include "board.h"
#include "esp_event_stubs.h"
#include "esp_idf_stubs.h"
#include "esp_ota_stubs.h"
#include "esp_task_wdt_stubs.h"
#include "freertos_stubs.h"
#include "gpio_stubs.h"
#include "i2c_stubs.h"
#include "input_monitor.h"
#include "mod_io.h"
#include "nvs_stubs.h"
#include "ota.h"
#include "relay.h"
#include "unity.h"

void test_device_config_suite(void);
void test_board_suite(void);
void test_input_monitor_suite(void);
void test_mod_io_suite(void);
void test_ota_suite(void);
void test_rest_api_suite(void);
void test_relay_suite(void);
void test_auth_suite(void);

void setUp(void)
{
    gpio_stub_reset();
    i2c_stub_reset();
    nvs_stub_reset();
    esp_event_stub_reset();
    esp_ota_stub_reset();
    esp_task_wdt_stub_reset();
    esp_stub_reset_restart_count();
    freertos_stub_reset();
    esp_stub_reset_time_override();
    board_reset_for_testing();
    device_config_reset_for_testing();
    input_monitor_reset_for_testing();
    mod_io_reset_for_testing();
    ota_reset_for_testing();
    relay_reset_for_testing();
}

void tearDown(void)
{
}

int main(void)
{
    UNITY_BEGIN();
    test_board_suite();
    test_device_config_suite();
    test_auth_suite();
    test_input_monitor_suite();
    test_mod_io_suite();
    test_ota_suite();
    test_rest_api_suite();
    test_relay_suite();
    return UNITY_END();
}

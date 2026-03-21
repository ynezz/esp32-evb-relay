#include <string.h>

#include "board.h"
#include "device_config.h"
#include "esp_event_stubs.h"
#include "esp_idf_stubs.h"
#include "esp_task_wdt_stubs.h"
#include "freertos_stubs.h"
#include "gpio_stubs.h"
#include "i2c_stubs.h"
#include "input_monitor.h"
#include "mod_io.h"
#include "relay_events.h"
#include "unity.h"

static i2c_master_bus_handle_t test_bus_handle(void)
{
    return (i2c_master_bus_handle_t)0x1;
}

static void queue_read_u8(uint8_t value)
{
    TEST_ASSERT_EQUAL(ESP_OK, i2c_stub_queue_read_data(&value, sizeof(value)));
}

static void queue_read_analog_u16(uint16_t value)
{
    uint8_t raw_value[2] = {0};

    raw_value[0] = (uint8_t)(value & 0xFFU);
    raw_value[1] = (uint8_t)((value >> 8U) & 0x03U);

    TEST_ASSERT_EQUAL(ESP_OK, i2c_stub_queue_read_data(raw_value, sizeof(raw_value)));
}

static void queue_snapshot(uint8_t digital_mask,
                           const uint16_t analog_values[MOD_IO_ANALOG_INPUT_COUNT])
{
    queue_read_u8(digital_mask);
    for (size_t i = 0; i < MOD_IO_ANALOG_INPUT_COUNT; ++i) {
        queue_read_analog_u16(analog_values[i]);
    }
}

static void init_present_mod_io(uint8_t relay_mask)
{
    queue_read_u8(relay_mask);
    TEST_ASSERT_EQUAL(ESP_OK, mod_io_init(test_bus_handle()));
}

static void init_absent_mod_io(void)
{
    i2c_stub_set_transmit_receive_result(ESP_ERR_NOT_FOUND);
    TEST_ASSERT_EQUAL(ESP_OK, mod_io_init(test_bus_handle()));
}

static void test_input_monitor_start_configures_button_isr_and_task(void)
{
    init_present_mod_io(0x00U);
    gpio_stub_set_input_level(BOARD_BUTTON, 1U);

    TEST_ASSERT_EQUAL(ESP_OK, input_monitor_start());
    TEST_ASSERT_EQUAL(ESP_OK, input_monitor_start());
    TEST_ASSERT_EQUAL_UINT32(1U, freertos_stub_get_task_create_count());
    TEST_ASSERT_EQUAL_STRING("input_monitor", freertos_stub_get_last_task_name());
    TEST_ASSERT_EQUAL_INT(GPIO_INTR_ANYEDGE, gpio_stub_get_intr_type(BOARD_BUTTON));
    TEST_ASSERT_TRUE(gpio_stub_has_isr_handler(BOARD_BUTTON));
}

static void test_input_monitor_start_cleans_up_button_isr_when_task_creation_fails(void)
{
    init_present_mod_io(0x00U);
    gpio_stub_set_input_level(BOARD_BUTTON, 1U);
    freertos_stub_set_task_create_result(pdFALSE);

    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, input_monitor_start());
    TEST_ASSERT_EQUAL_UINT32(0U, freertos_stub_get_task_create_count());
    TEST_ASSERT_EQUAL_INT(GPIO_INTR_DISABLE, gpio_stub_get_intr_type(BOARD_BUTTON));
    TEST_ASSERT_FALSE(gpio_stub_has_isr_handler(BOARD_BUTTON));
}

static void test_input_monitor_poll_once_publishes_initial_snapshot_events(void)
{
    static const uint16_t analog_values[MOD_IO_ANALOG_INPUT_COUNT] = {10U, 20U, 30U, 40U};
    evb_relay_analog_input_event_t analog_event = {0};
    input_monitor_snapshot_t snapshot = {0};

    init_present_mod_io(0x00U);
    gpio_stub_set_input_level(BOARD_BUTTON, 1U);
    TEST_ASSERT_EQUAL(ESP_OK, input_monitor_start());
    esp_event_stub_reset();

    queue_snapshot(0x03U, analog_values);
    esp_stub_set_time_us(1500000LL);
    TEST_ASSERT_EQUAL(ESP_OK, input_monitor_poll_once_for_testing());

    TEST_ASSERT_EQUAL(ESP_OK, input_monitor_get_snapshot(&snapshot));
    TEST_ASSERT_TRUE(snapshot.modio_present);
    TEST_ASSERT_TRUE(snapshot.sample_valid);
    TEST_ASSERT_EQUAL_HEX8(0x03U, snapshot.digital_mask);
    TEST_ASSERT_EQUAL_UINT16_ARRAY(analog_values, snapshot.analog_values, MOD_IO_ANALOG_INPUT_COUNT);
    TEST_ASSERT_EQUAL_UINT64(1500ULL, snapshot.sample_ts_ms);
    TEST_ASSERT_EQUAL_UINT32(8U, esp_event_stub_get_post_count());
    TEST_ASSERT_EQUAL(EVB_RELAY_EVENT, esp_event_stub_get_last_base());
    TEST_ASSERT_EQUAL_INT(EVB_RELAY_EVENT_ANALOG_INPUT, esp_event_stub_get_last_id());
    TEST_ASSERT_EQUAL_UINT32(sizeof(analog_event),
                             esp_event_stub_copy_last_data(&analog_event, sizeof(analog_event)));
    TEST_ASSERT_EQUAL_UINT8(4U, analog_event.id);
    TEST_ASSERT_EQUAL_UINT16(40U, analog_event.value);
    TEST_ASSERT_EQUAL_UINT64(1500ULL, analog_event.ts_ms);
}

static void test_input_monitor_publishes_digital_change_events(void)
{
    static const uint16_t analog_values[MOD_IO_ANALOG_INPUT_COUNT] = {100U, 100U, 100U, 100U};
    evb_relay_digital_input_event_t event = {0};

    init_present_mod_io(0x00U);
    gpio_stub_set_input_level(BOARD_BUTTON, 1U);
    TEST_ASSERT_EQUAL(ESP_OK, input_monitor_start());

    queue_snapshot(0x00U, analog_values);
    esp_stub_set_time_us(1000000LL);
    TEST_ASSERT_EQUAL(ESP_OK, input_monitor_poll_once_for_testing());

    esp_event_stub_reset();
    queue_snapshot(0x02U, analog_values);
    esp_stub_set_time_us(2000000LL);
    TEST_ASSERT_EQUAL(ESP_OK, input_monitor_poll_once_for_testing());

    TEST_ASSERT_EQUAL(EVB_RELAY_EVENT, esp_event_stub_get_last_base());
    TEST_ASSERT_EQUAL_INT(EVB_RELAY_EVENT_DIGITAL_INPUT, esp_event_stub_get_last_id());
    TEST_ASSERT_EQUAL_UINT32(sizeof(event),
                             esp_event_stub_copy_last_data(&event, sizeof(event)));
    TEST_ASSERT_EQUAL_UINT8(2U, event.id);
    TEST_ASSERT_TRUE(event.state);
    TEST_ASSERT_EQUAL_UINT64(2000ULL, event.ts_ms);
}

static void test_input_monitor_aggregates_analog_changes_against_threshold(void)
{
    static const uint16_t initial_analog[MOD_IO_ANALOG_INPUT_COUNT] = {100U, 100U, 100U, 100U};
    static const uint16_t below_threshold_analog[MOD_IO_ANALOG_INPUT_COUNT] = {105U, 100U, 100U, 100U};
    static const uint16_t above_threshold_analog[MOD_IO_ANALOG_INPUT_COUNT] = {109U, 100U, 100U, 100U};
    evb_relay_analog_input_event_t event = {0};

    init_present_mod_io(0x00U);
    gpio_stub_set_input_level(BOARD_BUTTON, 1U);
    TEST_ASSERT_EQUAL(ESP_OK, input_monitor_start());

    queue_snapshot(0x00U, initial_analog);
    esp_stub_set_time_us(1000000LL);
    TEST_ASSERT_EQUAL(ESP_OK, input_monitor_poll_once_for_testing());

    esp_event_stub_reset();
    queue_snapshot(0x00U, below_threshold_analog);
    esp_stub_set_time_us(2000000LL);
    TEST_ASSERT_EQUAL(ESP_OK, input_monitor_poll_once_for_testing());
    TEST_ASSERT_NULL(esp_event_stub_get_last_base());

    queue_snapshot(0x00U, above_threshold_analog);
    esp_stub_set_time_us(3000000LL);
    TEST_ASSERT_EQUAL(ESP_OK, input_monitor_poll_once_for_testing());

    TEST_ASSERT_EQUAL(EVB_RELAY_EVENT, esp_event_stub_get_last_base());
    TEST_ASSERT_EQUAL_INT(EVB_RELAY_EVENT_ANALOG_INPUT, esp_event_stub_get_last_id());
    TEST_ASSERT_EQUAL_UINT32(sizeof(event),
                             esp_event_stub_copy_last_data(&event, sizeof(event)));
    TEST_ASSERT_EQUAL_UINT8(1U, event.id);
    TEST_ASSERT_EQUAL_UINT16(109U, event.value);
    TEST_ASSERT_EQUAL_UINT64(3000ULL, event.ts_ms);
}

static void test_input_monitor_button_edges_are_debounced(void)
{
    static const uint16_t analog_values[MOD_IO_ANALOG_INPUT_COUNT] = {0U, 0U, 0U, 0U};
    evb_relay_button_event_t event = {0};

    init_present_mod_io(0x00U);
    gpio_stub_set_input_level(BOARD_BUTTON, 1U);
    TEST_ASSERT_EQUAL(ESP_OK, input_monitor_start());

    queue_snapshot(0x00U, analog_values);
    esp_stub_set_time_us(1000000LL);
    TEST_ASSERT_EQUAL(ESP_OK, input_monitor_poll_once_for_testing());

    esp_event_stub_reset();
    gpio_stub_set_input_level(BOARD_BUTTON, 0U);
    gpio_stub_trigger_isr(BOARD_BUTTON);
    queue_snapshot(0x00U, analog_values);
    esp_stub_set_time_us(1020000LL);
    TEST_ASSERT_EQUAL(ESP_OK, input_monitor_poll_once_for_testing());
    TEST_ASSERT_NULL(esp_event_stub_get_last_base());

    gpio_stub_set_input_level(BOARD_BUTTON, 1U);
    gpio_stub_trigger_isr(BOARD_BUTTON);
    queue_snapshot(0x00U, analog_values);
    esp_stub_set_time_us(1035000LL);
    TEST_ASSERT_EQUAL(ESP_OK, input_monitor_poll_once_for_testing());
    TEST_ASSERT_NULL(esp_event_stub_get_last_base());

    gpio_stub_set_input_level(BOARD_BUTTON, 0U);
    gpio_stub_trigger_isr(BOARD_BUTTON);
    queue_snapshot(0x00U, analog_values);
    esp_stub_set_time_us(1040000LL);
    TEST_ASSERT_EQUAL(ESP_OK, input_monitor_poll_once_for_testing());
    TEST_ASSERT_NULL(esp_event_stub_get_last_base());

    queue_snapshot(0x00U, analog_values);
    esp_stub_set_time_us(1095000LL);
    TEST_ASSERT_EQUAL(ESP_OK, input_monitor_poll_once_for_testing());

    TEST_ASSERT_EQUAL(EVB_RELAY_EVENT, esp_event_stub_get_last_base());
    TEST_ASSERT_EQUAL_INT(EVB_RELAY_EVENT_BUTTON, esp_event_stub_get_last_id());
    TEST_ASSERT_EQUAL_UINT32(sizeof(event),
                             esp_event_stub_copy_last_data(&event, sizeof(event)));
    TEST_ASSERT_TRUE(event.pressed);
    TEST_ASSERT_EQUAL_UINT64(1040ULL, event.ts_ms);
}

static void test_input_monitor_button_debounce_resamples_gpio_before_publishing(void)
{
    static const uint16_t analog_values[MOD_IO_ANALOG_INPUT_COUNT] = {0U, 0U, 0U, 0U};
    evb_relay_button_event_t event = {0};

    init_present_mod_io(0x00U);
    gpio_stub_set_input_level(BOARD_BUTTON, 1U);
    TEST_ASSERT_EQUAL(ESP_OK, input_monitor_start());

    queue_snapshot(0x00U, analog_values);
    esp_stub_set_time_us(1000000LL);
    TEST_ASSERT_EQUAL(ESP_OK, input_monitor_poll_once_for_testing());

    esp_event_stub_reset();
    gpio_stub_set_input_level(BOARD_BUTTON, 0U);
    gpio_stub_trigger_isr(BOARD_BUTTON);
    queue_snapshot(0x00U, analog_values);
    esp_stub_set_time_us(1020000LL);
    TEST_ASSERT_EQUAL(ESP_OK, input_monitor_poll_once_for_testing());
    TEST_ASSERT_NULL(esp_event_stub_get_last_base());

    gpio_stub_set_input_level(BOARD_BUTTON, 1U);
    queue_snapshot(0x00U, analog_values);
    esp_stub_set_time_us(1095000LL);
    TEST_ASSERT_EQUAL(ESP_OK, input_monitor_poll_once_for_testing());
    TEST_ASSERT_NULL(esp_event_stub_get_last_base());

    gpio_stub_set_input_level(BOARD_BUTTON, 0U);
    gpio_stub_trigger_isr(BOARD_BUTTON);
    queue_snapshot(0x00U, analog_values);
    esp_stub_set_time_us(1100000LL);
    TEST_ASSERT_EQUAL(ESP_OK, input_monitor_poll_once_for_testing());
    TEST_ASSERT_NULL(esp_event_stub_get_last_base());

    queue_snapshot(0x00U, analog_values);
    esp_stub_set_time_us(1155000LL);
    TEST_ASSERT_EQUAL(ESP_OK, input_monitor_poll_once_for_testing());

    TEST_ASSERT_EQUAL(EVB_RELAY_EVENT, esp_event_stub_get_last_base());
    TEST_ASSERT_EQUAL_INT(EVB_RELAY_EVENT_BUTTON, esp_event_stub_get_last_id());
    TEST_ASSERT_EQUAL_UINT32(sizeof(event),
                             esp_event_stub_copy_last_data(&event, sizeof(event)));
    TEST_ASSERT_TRUE(event.pressed);
    TEST_ASSERT_EQUAL_UINT64(1100ULL, event.ts_ms);
}

static void test_input_monitor_recovers_after_modio_absence(void)
{
    static const uint16_t analog_values[MOD_IO_ANALOG_INPUT_COUNT] = {1U, 2U, 3U, 4U};
    evb_relay_analog_input_event_t analog_event = {0};
    input_monitor_snapshot_t snapshot = {0};

    init_absent_mod_io();
    gpio_stub_set_input_level(BOARD_BUTTON, 1U);
    TEST_ASSERT_EQUAL(ESP_OK, input_monitor_start());

    esp_stub_set_time_us(1000000LL);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, input_monitor_poll_once_for_testing());
    TEST_ASSERT_EQUAL(ESP_OK, input_monitor_get_snapshot(&snapshot));
    TEST_ASSERT_FALSE(snapshot.modio_present);
    TEST_ASSERT_FALSE(snapshot.sample_valid);

    i2c_stub_set_transmit_receive_result(ESP_OK);
    esp_event_stub_reset();
    queue_read_u8(0x00U);
    queue_snapshot(0x01U, analog_values);
    esp_stub_set_time_us(2000000LL);
    TEST_ASSERT_EQUAL(ESP_OK, input_monitor_poll_once_for_testing());
    TEST_ASSERT_EQUAL(EVB_RELAY_EVENT, esp_event_stub_get_last_base());
    TEST_ASSERT_EQUAL_UINT32(9U, esp_event_stub_get_post_count());
    TEST_ASSERT_EQUAL_INT(EVB_RELAY_EVENT_ANALOG_INPUT, esp_event_stub_get_last_id());
    TEST_ASSERT_EQUAL_UINT32(sizeof(analog_event),
                             esp_event_stub_copy_last_data(&analog_event, sizeof(analog_event)));
    TEST_ASSERT_EQUAL_UINT8(4U, analog_event.id);
    TEST_ASSERT_EQUAL_UINT16(4U, analog_event.value);
    TEST_ASSERT_EQUAL_UINT64(2000ULL, analog_event.ts_ms);
    TEST_ASSERT_EQUAL(ESP_OK, input_monitor_get_snapshot(&snapshot));
    TEST_ASSERT_TRUE(snapshot.modio_present);
    TEST_ASSERT_TRUE(snapshot.sample_valid);
    TEST_ASSERT_EQUAL_HEX8(0x01U, snapshot.digital_mask);
    TEST_ASSERT_EQUAL_UINT16_ARRAY(analog_values, snapshot.analog_values, MOD_IO_ANALOG_INPUT_COUNT);
}

static void test_input_monitor_task_registers_and_feeds_task_watchdog(void)
{
    static const uint16_t analog_values[MOD_IO_ANALOG_INPUT_COUNT] = {1U, 2U, 3U, 4U};
    input_monitor_snapshot_t snapshot = {0};

    init_present_mod_io(0x00U);
    gpio_stub_set_input_level(BOARD_BUTTON, 1U);
    TEST_ASSERT_EQUAL(ESP_OK, input_monitor_start());

    queue_snapshot(0x01U, analog_values);
    esp_stub_set_time_us(1000000LL);
    TEST_ASSERT_EQUAL(ESP_OK, input_monitor_run_task_once_for_testing());

    TEST_ASSERT_EQUAL_UINT32(1U, (uint32_t)esp_task_wdt_stub_get_status_count());
    TEST_ASSERT_EQUAL_UINT32(1U, (uint32_t)esp_task_wdt_stub_get_add_count());
    TEST_ASSERT_EQUAL_UINT32(1U, (uint32_t)esp_task_wdt_stub_get_reset_count());
    TEST_ASSERT_EQUAL(ESP_OK, input_monitor_get_snapshot(&snapshot));
    TEST_ASSERT_TRUE(snapshot.sample_valid);
    TEST_ASSERT_EQUAL_HEX8(0x01U, snapshot.digital_mask);
    TEST_ASSERT_EQUAL_UINT16_ARRAY(analog_values, snapshot.analog_values, MOD_IO_ANALOG_INPUT_COUNT);
}

void test_input_monitor_suite(void)
{
    RUN_TEST(test_input_monitor_start_configures_button_isr_and_task);
    RUN_TEST(test_input_monitor_start_cleans_up_button_isr_when_task_creation_fails);
    RUN_TEST(test_input_monitor_poll_once_publishes_initial_snapshot_events);
    RUN_TEST(test_input_monitor_publishes_digital_change_events);
    RUN_TEST(test_input_monitor_aggregates_analog_changes_against_threshold);
    RUN_TEST(test_input_monitor_button_edges_are_debounced);
    RUN_TEST(test_input_monitor_button_debounce_resamples_gpio_before_publishing);
    RUN_TEST(test_input_monitor_recovers_after_modio_absence);
    RUN_TEST(test_input_monitor_task_registers_and_feeds_task_watchdog);
}

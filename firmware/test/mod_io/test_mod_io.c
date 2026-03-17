#include <string.h>

#include "i2c_stubs.h"
#include "mod_io.h"
#include "unity.h"

static i2c_master_bus_handle_t test_bus_handle(void)
{
    return (i2c_master_bus_handle_t)0x1;
}

static void assert_last_transaction_equals(const uint8_t *expected, size_t expected_size)
{
    size_t actual_size = 0;
    const uint8_t *actual = i2c_stub_get_last_transaction(&actual_size);

    TEST_ASSERT_EQUAL_UINT32(expected_size, actual_size);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, actual, expected_size);
}

static void assert_status(bool expected_present,
                          mod_io_relay_sync_t expected_sync,
                          uint8_t expected_mask)
{
    mod_io_status_t status = {0};

    TEST_ASSERT_EQUAL(ESP_OK, mod_io_get_status(&status));
    TEST_ASSERT_EQUAL(expected_present, status.present);
    TEST_ASSERT_EQUAL_INT(expected_sync, status.relay_sync);
    TEST_ASSERT_EQUAL_HEX8(expected_mask, status.relay_mask);
}

static void set_read_data_u8(uint8_t value)
{
    TEST_ASSERT_EQUAL(ESP_OK, i2c_stub_set_read_data(&value, sizeof(value)));
}

static void set_read_data_u16(uint8_t low, uint8_t high)
{
    const uint8_t bytes[2] = {low, high};

    TEST_ASSERT_EQUAL(ESP_OK, i2c_stub_set_read_data(bytes, sizeof(bytes)));
}

static void init_present_mod_io(uint8_t relay_mask)
{
    i2c_stub_set_probe_result(ESP_OK);
    set_read_data_u8(relay_mask);
    TEST_ASSERT_EQUAL(ESP_OK, mod_io_init(test_bus_handle()));
}

static void test_mod_io_requires_initialization(void)
{
    uint8_t relay_mask = 0;
    uint16_t analog_value = 0;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, mod_io_probe());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, mod_io_set_relays(0x01U));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, mod_io_set_relay(1U, true));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, mod_io_read_digital_inputs(&relay_mask));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, mod_io_read_analog_input(1U, &analog_value));
}

static void test_mod_io_init_reads_authoritative_relay_state(void)
{
    const uint8_t expected_transaction[] = {0x40U};
    uint8_t relay_mask = 0;
    mod_io_relay_sync_t relay_sync = MOD_IO_RELAY_SYNC_ABSENT;

    init_present_mod_io(0x05U);

    assert_last_transaction_equals(expected_transaction, sizeof(expected_transaction));
    assert_status(true, MOD_IO_RELAY_SYNC_SYNCHRONIZED, 0x05U);
    TEST_ASSERT_EQUAL(ESP_OK, mod_io_get_relays(&relay_mask, &relay_sync));
    TEST_ASSERT_EQUAL_HEX8(0x05U, relay_mask);
    TEST_ASSERT_EQUAL_INT(MOD_IO_RELAY_SYNC_SYNCHRONIZED, relay_sync);
}

static void test_mod_io_probe_transitions_absent_to_present_with_readback(void)
{
    const uint8_t expected_transaction[] = {0x40U};

    i2c_stub_set_probe_result(ESP_ERR_NOT_FOUND);
    TEST_ASSERT_EQUAL(ESP_OK, mod_io_init(test_bus_handle()));
    assert_status(false, MOD_IO_RELAY_SYNC_ABSENT, 0x00U);

    i2c_stub_set_probe_result(ESP_OK);
    set_read_data_u8(0x02U);
    TEST_ASSERT_EQUAL(ESP_OK, mod_io_probe());

    assert_last_transaction_equals(expected_transaction, sizeof(expected_transaction));
    assert_status(true, MOD_IO_RELAY_SYNC_SYNCHRONIZED, 0x02U);
}

static void test_mod_io_probe_keeps_absent_state_when_board_is_missing(void)
{
    i2c_stub_set_probe_result(ESP_ERR_NOT_FOUND);
    TEST_ASSERT_EQUAL(ESP_OK, mod_io_init(test_bus_handle()));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, mod_io_probe());
    assert_status(false, MOD_IO_RELAY_SYNC_ABSENT, 0x00U);
}

static void test_mod_io_set_relays_validates_mask_and_round_trips_via_readback(void)
{
    const uint8_t expected_write[] = {0x10U, 0x0FU};

    init_present_mod_io(0x00U);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, mod_io_set_relays(0x10U));
    TEST_ASSERT_EQUAL(ESP_OK, mod_io_set_relays(0x0FU));
    assert_last_transaction_equals(expected_write, sizeof(expected_write));
    assert_status(true, MOD_IO_RELAY_SYNC_SYNCHRONIZED, 0x0FU);

    set_read_data_u8(0x0FU);
    TEST_ASSERT_EQUAL(ESP_OK, mod_io_probe());
    assert_status(true, MOD_IO_RELAY_SYNC_SYNCHRONIZED, 0x0FU);
}

static void test_mod_io_set_relay_validates_ids_and_uses_read_modify_write(void)
{
    const uint8_t expected_write_on[] = {0x10U, 0x07U};
    const uint8_t expected_write_off[] = {0x10U, 0x06U};

    init_present_mod_io(0x05U);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, mod_io_set_relay(0U, true));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, mod_io_set_relay(5U, true));

    set_read_data_u8(0x05U);
    TEST_ASSERT_EQUAL(ESP_OK, mod_io_set_relay(2U, true));
    assert_last_transaction_equals(expected_write_on, sizeof(expected_write_on));
    assert_status(true, MOD_IO_RELAY_SYNC_SYNCHRONIZED, 0x07U);

    set_read_data_u8(0x07U);
    TEST_ASSERT_EQUAL(ESP_OK, mod_io_set_relay(1U, false));
    assert_last_transaction_equals(expected_write_off, sizeof(expected_write_off));
    assert_status(true, MOD_IO_RELAY_SYNC_SYNCHRONIZED, 0x06U);
}

static void test_mod_io_read_digital_inputs_uses_protocol_command(void)
{
    const uint8_t expected_transaction[] = {0x20U};
    uint8_t input_mask = 0;

    init_present_mod_io(0x00U);
    set_read_data_u8(0x1FU);

    TEST_ASSERT_EQUAL(ESP_OK, mod_io_read_digital_inputs(&input_mask));
    assert_last_transaction_equals(expected_transaction, sizeof(expected_transaction));
    TEST_ASSERT_EQUAL_HEX8(0x0FU, input_mask);
}

static void test_mod_io_read_analog_input_validates_ids_and_decodes_samples(void)
{
    const uint8_t expected_input_1[] = {0x30U};
    const uint8_t expected_input_2[] = {0x31U};
    const uint8_t expected_input_3[] = {0x32U};
    const uint8_t expected_input_4[] = {0x33U};
    uint16_t value = 0;

    init_present_mod_io(0x00U);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, mod_io_read_analog_input(0U, &value));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, mod_io_read_analog_input(5U, &value));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, mod_io_read_analog_input(1U, NULL));

    set_read_data_u16(0x80U, 0x00U);
    TEST_ASSERT_EQUAL(ESP_OK, mod_io_read_analog_input(1U, &value));
    assert_last_transaction_equals(expected_input_1, sizeof(expected_input_1));
    TEST_ASSERT_EQUAL_UINT16(1U, value);

    set_read_data_u16(0x01U, 0x00U);
    TEST_ASSERT_EQUAL(ESP_OK, mod_io_read_analog_input(2U, &value));
    assert_last_transaction_equals(expected_input_2, sizeof(expected_input_2));
    TEST_ASSERT_EQUAL_UINT16(128U, value);

    set_read_data_u16(0xFFU, 0x03U);
    TEST_ASSERT_EQUAL(ESP_OK, mod_io_read_analog_input(3U, &value));
    assert_last_transaction_equals(expected_input_3, sizeof(expected_input_3));
    TEST_ASSERT_EQUAL_UINT16(1023U, value);

    set_read_data_u16(0x80U, 0x00U);
    TEST_ASSERT_EQUAL(ESP_OK, mod_io_read_analog_input(4U, &value));
    assert_last_transaction_equals(expected_input_4, sizeof(expected_input_4));
    TEST_ASSERT_EQUAL_UINT16(1U, value);
}

static void test_mod_io_read_analog_inputs_reads_all_channels(void)
{
    const uint8_t expected_last_transaction[] = {0x33U};
    uint16_t values[MOD_IO_ANALOG_INPUT_COUNT] = {0};

    init_present_mod_io(0x00U);
    set_read_data_u16(0x01U, 0x00U);

    TEST_ASSERT_EQUAL(ESP_OK, mod_io_read_analog_inputs(values));
    assert_last_transaction_equals(expected_last_transaction, sizeof(expected_last_transaction));
    TEST_ASSERT_EQUAL_UINT16(128U, values[0]);
    TEST_ASSERT_EQUAL_UINT16(128U, values[1]);
    TEST_ASSERT_EQUAL_UINT16(128U, values[2]);
    TEST_ASSERT_EQUAL_UINT16(128U, values[3]);
}

static void test_mod_io_reprobe_after_write_error_restores_authoritative_state(void)
{
    const uint8_t expected_readback[] = {0x40U};

    init_present_mod_io(0x01U);
    i2c_stub_set_transmit_result(ESP_FAIL);
    set_read_data_u8(0x0AU);

    TEST_ASSERT_EQUAL(ESP_FAIL, mod_io_set_relays(0x0FU));
    assert_last_transaction_equals(expected_readback, sizeof(expected_readback));
    assert_status(true, MOD_IO_RELAY_SYNC_SYNCHRONIZED, 0x0AU);
}

static void test_mod_io_transaction_failure_marks_board_absent_when_reprobe_fails(void)
{
    init_present_mod_io(0x01U);
    i2c_stub_set_transmit_result(ESP_FAIL);
    i2c_stub_set_probe_result(ESP_ERR_NOT_FOUND);

    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, mod_io_set_relays(0x03U));
    assert_status(false, MOD_IO_RELAY_SYNC_ABSENT, 0x00U);
}

void test_mod_io_suite(void)
{
    RUN_TEST(test_mod_io_requires_initialization);
    RUN_TEST(test_mod_io_init_reads_authoritative_relay_state);
    RUN_TEST(test_mod_io_probe_transitions_absent_to_present_with_readback);
    RUN_TEST(test_mod_io_probe_keeps_absent_state_when_board_is_missing);
    RUN_TEST(test_mod_io_set_relays_validates_mask_and_round_trips_via_readback);
    RUN_TEST(test_mod_io_set_relay_validates_ids_and_uses_read_modify_write);
    RUN_TEST(test_mod_io_read_digital_inputs_uses_protocol_command);
    RUN_TEST(test_mod_io_read_analog_input_validates_ids_and_decodes_samples);
    RUN_TEST(test_mod_io_read_analog_inputs_reads_all_channels);
    RUN_TEST(test_mod_io_reprobe_after_write_error_restores_authoritative_state);
    RUN_TEST(test_mod_io_transaction_failure_marks_board_absent_when_reprobe_fails);
}

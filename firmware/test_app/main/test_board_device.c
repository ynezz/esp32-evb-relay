#include "board.h"
#include "unity.h"

TEST_CASE("board device init succeeds", "[qa][board][device]")
{
    TEST_ASSERT_EQUAL(ESP_OK, board_init());
}

TEST_CASE("board device init exposes i2c bus handle", "[qa][board][device]")
{
    TEST_ASSERT_EQUAL(ESP_OK, board_init());
    TEST_ASSERT_NOT_NULL(board_i2c_bus_handle());
}

TEST_CASE("board device init is idempotent", "[qa][board][device]")
{
    i2c_master_bus_handle_t first_handle;
    i2c_master_bus_handle_t second_handle;

    TEST_ASSERT_EQUAL(ESP_OK, board_init());
    first_handle = board_i2c_bus_handle();
    TEST_ASSERT_NOT_NULL(first_handle);

    TEST_ASSERT_EQUAL(ESP_OK, board_init());
    second_handle = board_i2c_bus_handle();
    TEST_ASSERT_NOT_NULL(second_handle);
    TEST_ASSERT_EQUAL_PTR(first_handle, second_handle);
}

TEST_CASE("board device config applies canonical i2c defaults", "[qa][board][device]")
{
    const i2c_device_config_t config = board_i2c_device_config(0x58U);

    TEST_ASSERT_EQUAL(I2C_ADDR_BIT_LEN_7, config.dev_addr_length);
    TEST_ASSERT_EQUAL_HEX16(0x58U, config.device_address);
    TEST_ASSERT_EQUAL_UINT32(BOARD_I2C_SCL_SPEED_HZ, config.scl_speed_hz);
    TEST_ASSERT_EQUAL_UINT32(0U, config.scl_wait_us);
    TEST_ASSERT_FALSE(config.flags.disable_ack_check);
}

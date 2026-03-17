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

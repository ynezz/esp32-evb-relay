#include "i2c_stubs.h"
#include "gpio_stubs.h"
#include "board.h"
#include "unity.h"

static void test_board_init_configures_i2c_bus_defaults(void)
{
    const i2c_master_bus_config_t *config;

    TEST_ASSERT_EQUAL(ESP_OK, board_init());
    TEST_ASSERT_NOT_NULL(board_i2c_bus_handle());

    config = i2c_stub_get_last_bus_config();
    TEST_ASSERT_NOT_NULL(config);
    TEST_ASSERT_EQUAL(BOARD_I2C_PORT, config->i2c_port);
    TEST_ASSERT_EQUAL(BOARD_I2C_SDA, config->sda_io_num);
    TEST_ASSERT_EQUAL(BOARD_I2C_SCL, config->scl_io_num);
    TEST_ASSERT_EQUAL(I2C_CLK_SRC_DEFAULT, config->clk_source);
    TEST_ASSERT_EQUAL_UINT8(7U, config->glitch_ignore_cnt);
    TEST_ASSERT_TRUE(config->flags.enable_internal_pullup);
}

static void test_board_reset_for_testing_clears_cached_bus_and_allows_reinit(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, board_init());
    TEST_ASSERT_NOT_NULL(board_i2c_bus_handle());

    board_reset_for_testing();
    TEST_ASSERT_NULL(board_i2c_bus_handle());

    TEST_ASSERT_EQUAL(ESP_OK, board_init());
    TEST_ASSERT_NOT_NULL(board_i2c_bus_handle());
}

void test_board_suite(void)
{
    RUN_TEST(test_board_init_configures_i2c_bus_defaults);
    RUN_TEST(test_board_reset_for_testing_clears_cached_bus_and_allows_reinit);
}

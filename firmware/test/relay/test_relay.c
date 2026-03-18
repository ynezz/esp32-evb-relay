#include "board.h"
#include "gpio_stubs.h"
#include "relay.h"
#include "unity.h"

static void assert_relay_state(uint8_t relay_id, gpio_num_t gpio_num, bool expected_on)
{
    bool actual_state = false;

    TEST_ASSERT_EQUAL(ESP_OK, relay_get(relay_id, &actual_state));
    TEST_ASSERT_EQUAL(expected_on, actual_state);
    TEST_ASSERT_EQUAL_UINT32(expected_on ? 1U : 0U, gpio_stub_get_level(gpio_num));
}

static void test_relay_rejects_invalid_ids(void)
{
    bool state = false;

    TEST_ASSERT_EQUAL(ESP_OK, relay_init());

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, relay_set(0U, true));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, relay_set(255U, true));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, relay_get(0U, &state));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, relay_get(255U, &state));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, relay_toggle(0U));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, relay_toggle(255U));

    TEST_ASSERT_EQUAL(ESP_OK, relay_set(1U, true));
    TEST_ASSERT_EQUAL(ESP_OK, relay_set(2U, true));
    assert_relay_state(1U, BOARD_RELAY1_GPIO, true);
    assert_relay_state(2U, BOARD_RELAY2_GPIO, true);
}

static void test_relay_requires_initialization(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, relay_set(1U, true));
}

static void test_relay_get_rejects_null_pointer(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, relay_init());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, relay_get(1U, NULL));
}

static void test_relay_init_is_idempotent_and_sets_relays_off(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, relay_init());
    TEST_ASSERT_TRUE(gpio_stub_is_configured(BOARD_RELAY1_GPIO));
    TEST_ASSERT_TRUE(gpio_stub_is_configured(BOARD_RELAY2_GPIO));
    TEST_ASSERT_EQUAL_INT(GPIO_MODE_INPUT_OUTPUT, gpio_stub_get_mode(BOARD_RELAY1_GPIO));
    TEST_ASSERT_EQUAL_INT(GPIO_MODE_INPUT_OUTPUT, gpio_stub_get_mode(BOARD_RELAY2_GPIO));
    TEST_ASSERT_EQUAL_UINT32(0U, gpio_stub_get_level(BOARD_RELAY1_GPIO));
    TEST_ASSERT_EQUAL_UINT32(0U, gpio_stub_get_level(BOARD_RELAY2_GPIO));

    TEST_ASSERT_EQUAL(ESP_OK, relay_init());
    TEST_ASSERT_EQUAL_UINT32(0U, gpio_stub_get_level(BOARD_RELAY1_GPIO));
    TEST_ASSERT_EQUAL_UINT32(0U, gpio_stub_get_level(BOARD_RELAY2_GPIO));
}

static void test_relay_set_updates_gpio_and_state(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, relay_init());

    TEST_ASSERT_EQUAL(ESP_OK, relay_set(1U, true));
    assert_relay_state(1U, BOARD_RELAY1_GPIO, true);
    TEST_ASSERT_EQUAL_UINT32(0U, gpio_stub_get_level(BOARD_RELAY2_GPIO));

    TEST_ASSERT_EQUAL(ESP_OK, relay_set(1U, false));
    assert_relay_state(1U, BOARD_RELAY1_GPIO, false);

    TEST_ASSERT_EQUAL(ESP_OK, relay_set(2U, true));
    assert_relay_state(2U, BOARD_RELAY2_GPIO, true);

    TEST_ASSERT_EQUAL(ESP_OK, relay_set(2U, false));
    assert_relay_state(2U, BOARD_RELAY2_GPIO, false);
}

static void test_relay_toggle_updates_gpio_and_state(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, relay_init());
    assert_relay_state(1U, BOARD_RELAY1_GPIO, false);

    TEST_ASSERT_EQUAL(ESP_OK, relay_toggle(1U));
    assert_relay_state(1U, BOARD_RELAY1_GPIO, true);

    TEST_ASSERT_EQUAL(ESP_OK, relay_toggle(1U));
    assert_relay_state(1U, BOARD_RELAY1_GPIO, false);
}

void test_relay_suite(void)
{
    RUN_TEST(test_relay_rejects_invalid_ids);
    RUN_TEST(test_relay_requires_initialization);
    RUN_TEST(test_relay_get_rejects_null_pointer);
    RUN_TEST(test_relay_init_is_idempotent_and_sets_relays_off);
    RUN_TEST(test_relay_set_updates_gpio_and_state);
    RUN_TEST(test_relay_toggle_updates_gpio_and_state);
}

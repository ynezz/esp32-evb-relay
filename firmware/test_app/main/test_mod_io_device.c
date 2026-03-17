#include <stddef.h>

#include "board.h"
#include "mod_io.h"
#include "unity.h"

static void require_mod_io_or_skip(void)
{
    esp_err_t err;

    err = board_init();
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = mod_io_init(board_i2c_bus_handle());
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = mod_io_probe();
    if (err == ESP_ERR_NOT_FOUND) {
        TEST_IGNORE_MESSAGE("MOD-IO board is not connected");
    }

    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_TRUE(mod_io_is_present());
}

TEST_CASE("mod_io device probe detects board", "[qa][mod_io][device]")
{
    mod_io_status_t status = {0};

    require_mod_io_or_skip();

    TEST_ASSERT_EQUAL(ESP_OK, mod_io_get_status(&status));
    TEST_ASSERT_TRUE(status.present);
    TEST_ASSERT_EQUAL(MOD_IO_RELAY_SYNC_SYNCHRONIZED, status.relay_sync);
    TEST_ASSERT_EQUAL_HEX8(0x00U, status.relay_mask & (uint8_t)~MOD_IO_RELAY_MASK_ALL);
}

TEST_CASE("mod_io device relay readback matches writes", "[qa][mod_io][device]")
{
    uint8_t relay_mask = 0;
    mod_io_relay_sync_t relay_sync = MOD_IO_RELAY_SYNC_ABSENT;

    require_mod_io_or_skip();

    TEST_ASSERT_EQUAL(ESP_OK, mod_io_set_relays(0x05U));
    TEST_ASSERT_EQUAL(ESP_OK, mod_io_probe());
    TEST_ASSERT_EQUAL(ESP_OK, mod_io_get_relays(&relay_mask, &relay_sync));
    TEST_ASSERT_EQUAL(MOD_IO_RELAY_SYNC_SYNCHRONIZED, relay_sync);
    TEST_ASSERT_EQUAL_HEX8(0x05U, relay_mask);

    TEST_ASSERT_EQUAL(ESP_OK, mod_io_set_relays(0x00U));
    TEST_ASSERT_EQUAL(ESP_OK, mod_io_probe());
    TEST_ASSERT_EQUAL(ESP_OK, mod_io_get_relays(&relay_mask, &relay_sync));
    TEST_ASSERT_EQUAL_HEX8(0x00U, relay_mask);
}

TEST_CASE("mod_io device digital inputs stay within low nibble", "[qa][mod_io][device]")
{
    uint8_t input_mask = 0;

    require_mod_io_or_skip();

    TEST_ASSERT_EQUAL(ESP_OK, mod_io_read_digital_inputs(&input_mask));
    TEST_ASSERT_EQUAL_HEX8(0x00U, input_mask & (uint8_t)~MOD_IO_RELAY_MASK_ALL);
}

TEST_CASE("mod_io device analog inputs stay within 10-bit range", "[qa][mod_io][device]")
{
    uint16_t values[MOD_IO_ANALOG_INPUT_COUNT] = {0};

    require_mod_io_or_skip();

    TEST_ASSERT_EQUAL(ESP_OK, mod_io_read_analog_inputs(values));
    for (size_t i = 0; i < MOD_IO_ANALOG_INPUT_COUNT; ++i) {
        TEST_ASSERT_LESS_OR_EQUAL_UINT32(1023U, values[i]);
    }
}

TEST_CASE("mod_io device all-off readback is authoritative", "[qa][mod_io][device]")
{
    uint8_t relay_mask = 0xFFU;
    mod_io_relay_sync_t relay_sync = MOD_IO_RELAY_SYNC_ABSENT;

    require_mod_io_or_skip();

    TEST_ASSERT_EQUAL(ESP_OK, mod_io_set_relays(0x00U));
    TEST_ASSERT_EQUAL(ESP_OK, mod_io_probe());
    TEST_ASSERT_EQUAL(ESP_OK, mod_io_get_relays(&relay_mask, &relay_sync));
    TEST_ASSERT_EQUAL(MOD_IO_RELAY_SYNC_SYNCHRONIZED, relay_sync);
    TEST_ASSERT_EQUAL_HEX8(0x00U, relay_mask);
}

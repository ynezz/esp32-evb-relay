#include "mod_io.h"
#include "relay.h"
#include "rest_api.h"
#include "unity.h"

void setUp(void)
{
}

void tearDown(void)
{
    uint8_t relay_mask = 0xFFU;
    mod_io_relay_sync_t relay_sync = MOD_IO_RELAY_SYNC_ABSENT;

    TEST_ASSERT_EQUAL(ESP_OK, rest_api_stop());
    TEST_ASSERT_EQUAL(ESP_OK, relay_init());
    TEST_ASSERT_EQUAL(ESP_OK, relay_set(1U, false));
    TEST_ASSERT_EQUAL(ESP_OK, relay_set(2U, false));

    if (!mod_io_is_present()) {
        return;
    }

    TEST_ASSERT_EQUAL(ESP_OK, mod_io_set_relays(0x00U));
    TEST_ASSERT_EQUAL(ESP_OK, mod_io_probe());
    TEST_ASSERT_EQUAL(ESP_OK, mod_io_get_relays(&relay_mask, &relay_sync));
    TEST_ASSERT_EQUAL(MOD_IO_RELAY_SYNC_SYNCHRONIZED, relay_sync);
    TEST_ASSERT_EQUAL_HEX8(0x00U, relay_mask);
}

TEST_CASE("test_app scaffold smoke test", "[qa][smoke]")
{
    TEST_ASSERT_EQUAL_INT(1, 1);
}

void app_main(void)
{
    /* pytest-embedded drives the Unity menu over the serial console. */
    unity_run_menu();
}

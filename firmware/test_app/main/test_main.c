#include "mod_io.h"
#include "relay.h"
#include "rest_api.h"
#include "unity.h"

void setUp(void)
{
}

void tearDown(void)
{
    (void)rest_api_stop();

    if (relay_init() == ESP_OK) {
        (void)relay_set(1U, false);
        (void)relay_set(2U, false);
    }

    if (!mod_io_is_present()) {
        return;
    }

    (void)mod_io_set_relays(0x00U);
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

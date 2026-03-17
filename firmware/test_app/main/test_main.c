#include "unity.h"

TEST_CASE("test_app scaffold smoke test", "[qa][smoke]")
{
    TEST_ASSERT_EQUAL_INT(1, 1);
}

void app_main(void)
{
    /* pytest-embedded drives the Unity menu over the serial console. */
    unity_run_menu();
}

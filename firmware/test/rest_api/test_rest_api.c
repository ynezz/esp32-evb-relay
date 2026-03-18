#include "rest_api.h"

#include "unity.h"

static void test_rest_api_modio_sync_from_driver_maps_absent(void)
{
    TEST_ASSERT_EQUAL_INT(REST_API_MODIO_SYNC_ABSENT,
                          rest_api_modio_sync_from_driver(MOD_IO_RELAY_SYNC_ABSENT));
}

static void test_rest_api_modio_sync_from_driver_maps_synchronized(void)
{
    TEST_ASSERT_EQUAL_INT(REST_API_MODIO_SYNC_SYNCHRONIZED,
                          rest_api_modio_sync_from_driver(MOD_IO_RELAY_SYNC_SYNCHRONIZED));
}

static void test_rest_api_modio_sync_from_driver_maps_unknown_to_absent(void)
{
    TEST_ASSERT_EQUAL_INT(REST_API_MODIO_SYNC_ABSENT,
                          rest_api_modio_sync_from_driver((mod_io_relay_sync_t)99));
}

void test_rest_api_suite(void)
{
    RUN_TEST(test_rest_api_modio_sync_from_driver_maps_absent);
    RUN_TEST(test_rest_api_modio_sync_from_driver_maps_synchronized);
    RUN_TEST(test_rest_api_modio_sync_from_driver_maps_unknown_to_absent);
}

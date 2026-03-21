#include <stdio.h>
#include <stdlib.h>

#include "mod_io.h"
#include "esp_ota_ops.h"
#include "ota.h"
#include "relay.h"
#include "rest_api.h"
#include "unity.h"

void test_rest_api_device_cleanup(void);

void setUp(void)
{
}

void tearDown(void)
{
    test_rest_api_device_cleanup();
    (void)rest_api_stop();
    ota_reset_for_testing();

    {
        const esp_partition_t *running_partition = esp_ota_get_running_partition();

        if (running_partition != NULL) {
            (void)esp_ota_set_boot_partition(running_partition);
        }
    }

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
    esp_err_t err = ota_confirm_running_image_if_pending();

    if (err != ESP_OK) {
        printf("Failed to confirm running OTA image: %s\n", esp_err_to_name(err));
        abort();
    }

    /* pytest-embedded drives the Unity menu over the serial console. */
    unity_run_menu();
}

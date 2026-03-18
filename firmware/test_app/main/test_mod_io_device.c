#include <stddef.h>
#include <string.h>

#include "board.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mod_io.h"
#include "rest_api_events.h"
#include "unity.h"

typedef struct {
    bool received;
    evb_relay_relay_changed_event_t event;
} mod_io_event_capture_t;

static void mod_io_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                                 void *event_data)
{
    mod_io_event_capture_t *capture = (mod_io_event_capture_t *)arg;

    if ((capture == NULL) || (event_base != EVB_RELAY_EVENT) ||
            (event_id != EVB_RELAY_EVENT_RELAY_CHANGED) || (event_data == NULL)) {
        return;
    }

    capture->received = true;
    memcpy(&capture->event, event_data, sizeof(capture->event));
}

static void ensure_default_event_loop(void)
{
    esp_err_t err = esp_event_loop_create_default();

    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        TEST_FAIL_MESSAGE("Failed to create default event loop");
    }
}

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
    TEST_ASSERT_EQUAL_HEX8(0x00U, input_mask & (uint8_t)~MOD_IO_DIGITAL_INPUT_MASK_ALL);
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

TEST_CASE("mod_io device publishes relay_changed event after write", "[qa][mod_io][device]")
{
    esp_event_handler_instance_t handler_instance = NULL;
    mod_io_event_capture_t capture = {0};
    volatile bool handler_registered = false;
    volatile bool mod_io_ready = false;

    ensure_default_event_loop();
    if (TEST_PROTECT()) {
        require_mod_io_or_skip();
        mod_io_ready = true;

        TEST_ASSERT_EQUAL(ESP_OK, mod_io_set_relays(0x00U));
        TEST_ASSERT_EQUAL(ESP_OK,
                          esp_event_handler_instance_register(EVB_RELAY_EVENT,
                                                              EVB_RELAY_EVENT_RELAY_CHANGED,
                                                              mod_io_event_handler, &capture,
                                                              &handler_instance));
        handler_registered = true;

        TEST_ASSERT_EQUAL(ESP_OK, mod_io_set_relays(0x01U));
        for (int i = 0; (i < 20) && !capture.received; ++i) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        TEST_ASSERT_TRUE(capture.received);
        TEST_ASSERT_EQUAL(EVB_RELAY_RELAY_GROUP_MODIO, capture.event.group);
        TEST_ASSERT_EQUAL_UINT8(1U, capture.event.id);
        TEST_ASSERT_TRUE(capture.event.state);
        TEST_ASSERT_TRUE(capture.event.ts_ms > 0U);
    }

    if (handler_registered) {
        TEST_ASSERT_EQUAL(ESP_OK,
                          esp_event_handler_instance_unregister(EVB_RELAY_EVENT,
                                                                EVB_RELAY_EVENT_RELAY_CHANGED,
                                                                handler_instance));
    }

    if (mod_io_ready) {
        TEST_ASSERT_EQUAL(ESP_OK, mod_io_set_relays(0x00U));
    }
}

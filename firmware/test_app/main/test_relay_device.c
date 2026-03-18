#include <string.h>

#include "board.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "relay.h"
#include "relay_events.h"
#include "unity.h"

typedef struct {
    bool received;
    evb_relay_relay_changed_event_t event;
} relay_event_capture_t;

static void relay_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                                void *event_data)
{
    relay_event_capture_t *capture = (relay_event_capture_t *)arg;

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

TEST_CASE("relay device init sets onboard relays off", "[qa][relay][device]")
{
    bool relay1_state = true;
    bool relay2_state = true;

    TEST_ASSERT_EQUAL(ESP_OK, board_init());
    TEST_ASSERT_EQUAL(ESP_OK, relay_init());
    TEST_ASSERT_EQUAL(ESP_OK, relay_get(1U, &relay1_state));
    TEST_ASSERT_EQUAL(ESP_OK, relay_get(2U, &relay2_state));
    TEST_ASSERT_FALSE(relay1_state);
    TEST_ASSERT_FALSE(relay2_state);
    TEST_ASSERT_EQUAL(0, gpio_get_level(BOARD_RELAY1_GPIO));
    TEST_ASSERT_EQUAL(0, gpio_get_level(BOARD_RELAY2_GPIO));
}

TEST_CASE("relay device toggles real gpio level", "[qa][relay][device]")
{
    TEST_ASSERT_EQUAL(ESP_OK, board_init());
    TEST_ASSERT_EQUAL(ESP_OK, relay_init());

    TEST_ASSERT_EQUAL(ESP_OK, relay_set(1U, true));
    TEST_ASSERT_EQUAL(1, gpio_get_level(BOARD_RELAY1_GPIO));
    TEST_ASSERT_EQUAL(0, gpio_get_level(BOARD_RELAY2_GPIO));
    vTaskDelay(pdMS_TO_TICKS(50));

    TEST_ASSERT_EQUAL(ESP_OK, relay_set(1U, false));
    TEST_ASSERT_EQUAL(0, gpio_get_level(BOARD_RELAY1_GPIO));
}

TEST_CASE("relay device publishes change event", "[qa][relay][device]")
{
    esp_event_handler_instance_t handler_instance = NULL;
    relay_event_capture_t capture = {0};

    TEST_ASSERT_EQUAL(ESP_OK, board_init());
    TEST_ASSERT_EQUAL(ESP_OK, relay_init());
    ensure_default_event_loop();
    TEST_ASSERT_EQUAL(ESP_OK,
                      esp_event_handler_instance_register(EVB_RELAY_EVENT,
                                                          EVB_RELAY_EVENT_RELAY_CHANGED,
                                                          relay_event_handler, &capture,
                                                          &handler_instance));

    TEST_ASSERT_EQUAL(ESP_OK, relay_toggle(1U));
    for (int i = 0; (i < 20) && !capture.received; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    TEST_ASSERT_TRUE(capture.received);
    TEST_ASSERT_EQUAL(EVB_RELAY_RELAY_GROUP_ONBOARD, capture.event.group);
    TEST_ASSERT_EQUAL_UINT8(1U, capture.event.id);
    TEST_ASSERT_TRUE(capture.event.state);
    TEST_ASSERT_TRUE(capture.event.ts_ms > 0U);

    TEST_ASSERT_EQUAL(ESP_OK,
                      esp_event_handler_instance_unregister(EVB_RELAY_EVENT,
                                                            EVB_RELAY_EVENT_RELAY_CHANGED,
                                                            handler_instance));
}

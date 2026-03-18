#include "input_monitor.h"

#include <inttypes.h>
#include <string.h>

#include "sdkconfig.h"
#include "board.h"
#include "device_config.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "relay_events.h"

#define INPUT_MONITOR_TASK_STACK_WORDS 4096U
#define INPUT_MONITOR_TASK_PRIORITY 5U
#define INPUT_MONITOR_BUTTON_PRESSED_LEVEL 0

typedef struct {
    bool started;
    bool modio_present;
    bool snapshot_valid;
    bool button_state_valid;
    bool button_stable_pressed;
    bool button_candidate_valid;
    bool button_candidate_pressed;
    uint8_t digital_mask;
    uint16_t analog_values[MOD_IO_ANALOG_INPUT_COUNT];
    uint16_t analog_event_baseline[MOD_IO_ANALOG_INPUT_COUNT];
    uint64_t sample_ts_ms;
    uint64_t last_poll_ts_ms;
    uint64_t button_candidate_since_ms;
    TaskHandle_t task_handle;
} input_monitor_state_t;

static const char *TAG = "input_monitor";

static StaticSemaphore_t s_lock_buffer;
static SemaphoreHandle_t s_lock;
static input_monitor_state_t s_state;
static volatile bool s_button_irq_pending;

static uint64_t input_monitor_timestamp_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000LL);
}

static esp_err_t input_monitor_ensure_lock(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buffer);
    }

    return (s_lock != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t input_monitor_lock(void)
{
    ESP_RETURN_ON_ERROR(input_monitor_ensure_lock(), TAG, "Failed to create input monitor mutex");

    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

static void input_monitor_unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

static bool input_monitor_button_pressed(void)
{
    return gpio_get_level(BOARD_BUTTON) == INPUT_MONITOR_BUTTON_PRESSED_LEVEL;
}

static uint16_t input_monitor_abs_diff_u16(uint16_t left, uint16_t right)
{
    return (left >= right) ? (uint16_t)(left - right) : (uint16_t)(right - left);
}

static uint32_t input_monitor_poll_interval_ms(void)
{
    uint32_t poll_interval_ms = DEVICE_CONFIG_DEFAULT_POLL_INTERVAL_MS;

    if (device_config_get_poll_interval_ms(&poll_interval_ms) != ESP_OK) {
        return DEVICE_CONFIG_DEFAULT_POLL_INTERVAL_MS;
    }

    return poll_interval_ms;
}

#if CONFIG_ESP_TASK_WDT_EN
static bool input_monitor_task_watchdog_register(void)
{
    esp_err_t err = esp_task_wdt_status(NULL);

    if (err == ESP_OK) {
        return true;
    }

    if (err == ESP_ERR_NOT_FOUND) {
        err = esp_task_wdt_add(NULL);
        if (err == ESP_OK) {
            return true;
        }
    }

    if (err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Failed to register input monitor with task watchdog: %s",
                 esp_err_to_name(err));
    }

    return false;
}

static bool input_monitor_task_watchdog_reset(bool registered)
{
    esp_err_t err;

    if (!registered) {
        return false;
    }

    err = esp_task_wdt_reset();
    if (err == ESP_OK) {
        return true;
    }

    if ((err != ESP_ERR_INVALID_STATE) && (err != ESP_ERR_NOT_FOUND)) {
        ESP_LOGW(TAG, "Failed to feed input monitor task watchdog: %s", esp_err_to_name(err));
    }

    return false;
}
#else
static bool input_monitor_task_watchdog_register(void)
{
    return false;
}

static bool input_monitor_task_watchdog_reset(bool registered)
{
    (void)registered;
    return false;
}
#endif

static void input_monitor_publish_event(int32_t event_id,
                                        const void *event_data,
                                        size_t event_data_size,
                                        const char *event_name)
{
    esp_err_t err = esp_event_post(EVB_RELAY_EVENT, event_id, event_data, event_data_size, 0);

    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGD(TAG, "Skipping %s event because the default event loop is not ready", event_name);
        return;
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to publish %s event: %s", event_name, esp_err_to_name(err));
    }
}

static void input_monitor_publish_digital_event(const evb_relay_digital_input_event_t *event)
{
    input_monitor_publish_event(EVB_RELAY_EVENT_DIGITAL_INPUT, event, sizeof(*event), "digital_input");
}

static void input_monitor_publish_analog_event(const evb_relay_analog_input_event_t *event)
{
    input_monitor_publish_event(EVB_RELAY_EVENT_ANALOG_INPUT, event, sizeof(*event), "analog_input");
}

static void input_monitor_publish_button_event(const evb_relay_button_event_t *event)
{
    input_monitor_publish_event(EVB_RELAY_EVENT_BUTTON, event, sizeof(*event), "button");
}

static void input_monitor_publish_modio_presence_event(
    const evb_relay_modio_presence_event_t *event)
{
    input_monitor_publish_event(EVB_RELAY_EVENT_MODIO_PRESENCE, event, sizeof(*event),
                                "modio_presence");
}

static void input_monitor_capture_initial_button_state_locked(void)
{
    if (s_state.button_state_valid) {
        return;
    }

    s_state.button_stable_pressed = input_monitor_button_pressed();
    s_state.button_state_valid = true;
    s_state.button_candidate_valid = false;
}

static void input_monitor_update_modio_presence_locked(bool modio_present,
                                                       uint64_t ts_ms,
                                                       bool *publish_event,
                                                       evb_relay_modio_presence_event_t *event)
{
    if (publish_event != NULL) {
        *publish_event = false;
    }
    if (event != NULL) {
        memset(event, 0, sizeof(*event));
    }

    if (s_state.modio_present == modio_present) {
        return;
    }

    s_state.modio_present = modio_present;
    if (!modio_present) {
        s_state.snapshot_valid = false;
        s_state.digital_mask = 0U;
        memset(s_state.analog_values, 0, sizeof(s_state.analog_values));
        memset(s_state.analog_event_baseline, 0, sizeof(s_state.analog_event_baseline));
        s_state.sample_ts_ms = 0U;
    }

    if ((publish_event != NULL) && (event != NULL)) {
        *publish_event = true;
        *event = (evb_relay_modio_presence_event_t) {
            .present = modio_present,
            .ts_ms = ts_ms,
        };
    }

    ESP_LOGI(TAG, "MOD-IO presence changed: %s", modio_present ? "present" : "absent");
}

static void input_monitor_sync_modio_presence_locked(uint64_t now_ms,
                                                     bool *publish_event,
                                                     evb_relay_modio_presence_event_t *event)
{
    mod_io_status_t status = {0};

    if (mod_io_get_status(&status) == ESP_OK) {
        input_monitor_update_modio_presence_locked(status.present, now_ms, publish_event, event);
    }
}

static void input_monitor_record_snapshot_locked(uint8_t digital_mask,
                                                 const uint16_t analog_values[MOD_IO_ANALOG_INPUT_COUNT],
                                                 uint64_t sample_ts_ms)
{
    s_state.modio_present = true;
    s_state.snapshot_valid = true;
    s_state.digital_mask = digital_mask;
    memcpy(s_state.analog_values, analog_values, sizeof(s_state.analog_values));
    s_state.sample_ts_ms = sample_ts_ms;
}

static void input_monitor_collect_input_events_locked(
    uint8_t digital_mask,
    const uint16_t analog_values[MOD_IO_ANALOG_INPUT_COUNT],
    uint64_t sample_ts_ms,
    evb_relay_digital_input_event_t digital_events[MOD_IO_DIGITAL_INPUT_COUNT],
    size_t *digital_event_count,
    evb_relay_analog_input_event_t analog_events[MOD_IO_ANALOG_INPUT_COUNT],
    size_t *analog_event_count)
{
    bool had_snapshot = s_state.snapshot_valid && s_state.modio_present;

    *digital_event_count = 0U;
    *analog_event_count = 0U;

    if (had_snapshot) {
        uint8_t changed_digital_mask = (uint8_t)(s_state.digital_mask ^ digital_mask);

        for (uint8_t input_id = 1U; input_id <= MOD_IO_DIGITAL_INPUT_COUNT; ++input_id) {
            uint8_t input_bit = (uint8_t)(1U << (input_id - 1U));

            if ((changed_digital_mask & input_bit) == 0U) {
                continue;
            }

            digital_events[*digital_event_count] = (evb_relay_digital_input_event_t) {
                .id = input_id,
                .state = (digital_mask & input_bit) != 0U,
                .ts_ms = sample_ts_ms,
            };
            ++(*digital_event_count);
        }

        for (uint8_t input_id = 1U; input_id <= MOD_IO_ANALOG_INPUT_COUNT; ++input_id) {
            size_t index = input_id - 1U;

            if (input_monitor_abs_diff_u16(analog_values[index], s_state.analog_event_baseline[index]) <
                    INPUT_MONITOR_DEFAULT_ANALOG_CHANGE_THRESHOLD) {
                continue;
            }

            analog_events[*analog_event_count] = (evb_relay_analog_input_event_t) {
                .id = input_id,
                .value = analog_values[index],
                .ts_ms = sample_ts_ms,
            };
            s_state.analog_event_baseline[index] = analog_values[index];
            ++(*analog_event_count);
        }
    } else {
        memcpy(s_state.analog_event_baseline, analog_values, sizeof(s_state.analog_event_baseline));
    }

    input_monitor_record_snapshot_locked(digital_mask, analog_values, sample_ts_ms);
}

static void input_monitor_collect_button_event_locked(uint64_t now_ms,
                                                      bool *publish_event,
                                                      evb_relay_button_event_t *event)
{
    bool edge_pending = s_button_irq_pending;

    *publish_event = false;
    if (event != NULL) {
        memset(event, 0, sizeof(*event));
    }

    input_monitor_capture_initial_button_state_locked();

    if (edge_pending) {
        s_button_irq_pending = false;
        s_state.button_candidate_pressed = input_monitor_button_pressed();
        s_state.button_candidate_since_ms = now_ms;
        s_state.button_candidate_valid = true;
    }

    if (!s_state.button_candidate_valid) {
        return;
    }

    if (s_state.button_candidate_pressed == s_state.button_stable_pressed) {
        s_state.button_candidate_valid = false;
        return;
    }

    if ((now_ms - s_state.button_candidate_since_ms) < INPUT_MONITOR_DEFAULT_BUTTON_DEBOUNCE_MS) {
        return;
    }

    s_state.button_stable_pressed = s_state.button_candidate_pressed;
    s_state.button_candidate_valid = false;
    *publish_event = true;
    *event = (evb_relay_button_event_t) {
        .pressed = s_state.button_stable_pressed,
        .ts_ms = s_state.button_candidate_since_ms,
    };
}

static esp_err_t input_monitor_sample_modio(uint64_t now_ms, bool publish_events)
{
    evb_relay_digital_input_event_t digital_events[MOD_IO_DIGITAL_INPUT_COUNT];
    evb_relay_analog_input_event_t analog_events[MOD_IO_ANALOG_INPUT_COUNT];
    evb_relay_modio_presence_event_t presence_event = {0};
    size_t digital_event_count = 0U;
    size_t analog_event_count = 0U;
    uint8_t digital_mask = 0U;
    uint16_t analog_values[MOD_IO_ANALOG_INPUT_COUNT] = {0};
    bool publish_presence_event = false;
    esp_err_t err;

    memset(digital_events, 0, sizeof(digital_events));
    memset(analog_events, 0, sizeof(analog_events));

    err = mod_io_read_digital_inputs(&digital_mask);
    if (err == ESP_OK) {
        err = mod_io_read_analog_inputs(analog_values);
    }

    ESP_RETURN_ON_ERROR(input_monitor_lock(), TAG, "Failed to lock input monitor state");
    if (!s_state.started) {
        input_monitor_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    if (err == ESP_OK) {
        input_monitor_update_modio_presence_locked(true, now_ms, &publish_presence_event,
                                                   &presence_event);
        input_monitor_collect_input_events_locked(digital_mask, analog_values, now_ms, digital_events,
                                                  &digital_event_count, analog_events,
                                                  &analog_event_count);
    } else {
        input_monitor_sync_modio_presence_locked(now_ms, &publish_presence_event, &presence_event);
    }
    input_monitor_unlock();

    if (publish_events && publish_presence_event) {
        input_monitor_publish_modio_presence_event(&presence_event);
    }
    if (publish_events) {
        for (size_t i = 0; i < digital_event_count; ++i) {
            input_monitor_publish_digital_event(&digital_events[i]);
        }
        for (size_t i = 0; i < analog_event_count; ++i) {
            input_monitor_publish_analog_event(&analog_events[i]);
        }
    }

    return err;
}

static void input_monitor_process_button(uint64_t now_ms, bool publish_events)
{
    evb_relay_button_event_t button_event = {0};
    bool publish_button_event = false;

    if (input_monitor_lock() != ESP_OK) {
        return;
    }

    if (!s_state.started) {
        input_monitor_unlock();
        return;
    }

    input_monitor_collect_button_event_locked(now_ms, &publish_button_event, &button_event);
    input_monitor_unlock();

    if (publish_events && publish_button_event) {
        input_monitor_publish_button_event(&button_event);
    }
}

static bool input_monitor_poll_due(uint64_t now_ms)
{
    if (s_state.last_poll_ts_ms == 0U) {
        return true;
    }

    return (now_ms - s_state.last_poll_ts_ms) >= input_monitor_poll_interval_ms();
}

static uint32_t input_monitor_wait_ms_until_next_work(uint64_t now_ms)
{
    uint32_t wait_ms = input_monitor_poll_interval_ms();

    if (s_state.last_poll_ts_ms != 0U) {
        uint64_t elapsed_ms = now_ms - s_state.last_poll_ts_ms;

        wait_ms = (elapsed_ms >= wait_ms) ? 0U : (uint32_t)(wait_ms - elapsed_ms);
    }

    if (s_state.button_candidate_valid) {
        uint32_t debounce_wait_ms;
        uint64_t deadline_ms =
            s_state.button_candidate_since_ms + INPUT_MONITOR_DEFAULT_BUTTON_DEBOUNCE_MS;

        debounce_wait_ms = (now_ms >= deadline_ms) ? 0U : (uint32_t)(deadline_ms - now_ms);
        if (debounce_wait_ms < wait_ms) {
            wait_ms = debounce_wait_ms;
        }
    }

    return wait_ms;
}

static void input_monitor_button_isr(void *arg)
{
    BaseType_t higher_priority_task_woken = pdFALSE;

    (void)arg;
    s_button_irq_pending = true;
    if (s_state.task_handle != NULL) {
        vTaskNotifyGiveFromISR(s_state.task_handle, &higher_priority_task_woken);
    }
}

static uint32_t input_monitor_task_process_iteration(void)
{
    uint64_t now_ms = input_monitor_timestamp_ms();

    input_monitor_process_button(now_ms, true);
    if (input_monitor_poll_due(now_ms)) {
        s_state.last_poll_ts_ms = now_ms;
        (void)input_monitor_sample_modio(now_ms, true);
    }

    now_ms = input_monitor_timestamp_ms();
    return input_monitor_wait_ms_until_next_work(now_ms);
}

static void input_monitor_task(void *arg)
{
    bool watchdog_registered;

    (void)arg;
    watchdog_registered = input_monitor_task_watchdog_register();

    for (;;) {
        uint32_t wait_ms = input_monitor_task_process_iteration();

        watchdog_registered = input_monitor_task_watchdog_reset(watchdog_registered);
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
    }
}

esp_err_t input_monitor_start(void)
{
    BaseType_t task_result;
    mod_io_status_t mod_io_status = {0};
    esp_err_t err;

    err = mod_io_get_status(&mod_io_status);
    if (err != ESP_OK) {
        return err;
    }

    ESP_RETURN_ON_ERROR(input_monitor_lock(), TAG, "Failed to lock input monitor state");
    if (s_state.started) {
        input_monitor_unlock();
        return ESP_OK;
    }

    s_state.modio_present = mod_io_status.present;
    s_state.snapshot_valid = false;
    s_state.digital_mask = 0U;
    memset(s_state.analog_values, 0, sizeof(s_state.analog_values));
    memset(s_state.analog_event_baseline, 0, sizeof(s_state.analog_event_baseline));
    s_state.sample_ts_ms = 0U;
    s_state.last_poll_ts_ms = 0U;
    s_button_irq_pending = false;
    input_monitor_capture_initial_button_state_locked();
    input_monitor_unlock();

    err = gpio_install_isr_service(0);
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return err;
    }

    err = gpio_set_intr_type(BOARD_BUTTON, GPIO_INTR_ANYEDGE);
    if (err != ESP_OK) {
        return err;
    }

    err = gpio_isr_handler_add(BOARD_BUTTON, input_monitor_button_isr, NULL);
    if (err != ESP_OK) {
        return err;
    }

    ESP_RETURN_ON_ERROR(input_monitor_lock(), TAG, "Failed to lock input monitor state");
    task_result = xTaskCreate(input_monitor_task, "input_monitor", INPUT_MONITOR_TASK_STACK_WORDS,
                              NULL, INPUT_MONITOR_TASK_PRIORITY, &s_state.task_handle);
    if (task_result != pdPASS) {
        s_state.task_handle = NULL;
        input_monitor_unlock();
        return ESP_ERR_NO_MEM;
    }

    s_state.started = true;
    input_monitor_unlock();

    ESP_LOGI(TAG, "Started input monitor task with %" PRIu32 "ms polling",
             input_monitor_poll_interval_ms());
    return ESP_OK;
}

esp_err_t input_monitor_get_snapshot(input_monitor_snapshot_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "Snapshot output buffer is required");
    ESP_RETURN_ON_ERROR(input_monitor_lock(), TAG, "Failed to lock input monitor state");

    if (!s_state.started) {
        input_monitor_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    out->modio_present = s_state.modio_present;
    out->sample_valid = s_state.snapshot_valid;
    out->digital_mask = s_state.digital_mask;
    memcpy(out->analog_values, s_state.analog_values, sizeof(out->analog_values));
    out->sample_ts_ms = s_state.sample_ts_ms;
    input_monitor_unlock();

    return ESP_OK;
}

#if defined(UNIT_TEST) || defined(INPUT_MONITOR_ENABLE_TESTING_API)
esp_err_t input_monitor_poll_once_for_testing(void)
{
    uint64_t now_ms;
    esp_err_t err;

    ESP_RETURN_ON_ERROR(input_monitor_lock(), TAG, "Failed to lock input monitor state");
    if (!s_state.started) {
        input_monitor_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    input_monitor_unlock();

    now_ms = input_monitor_timestamp_ms();
    input_monitor_process_button(now_ms, true);
    err = input_monitor_sample_modio(now_ms, true);

    if (input_monitor_lock() == ESP_OK) {
        s_state.last_poll_ts_ms = now_ms;
        input_monitor_unlock();
    }

    return err;
}

esp_err_t input_monitor_run_task_once_for_testing(void)
{
    bool watchdog_registered;
    uint32_t wait_ms;

    ESP_RETURN_ON_ERROR(input_monitor_lock(), TAG, "Failed to lock input monitor state");
    if (!s_state.started) {
        input_monitor_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    input_monitor_unlock();

    watchdog_registered = input_monitor_task_watchdog_register();
    wait_ms = input_monitor_task_process_iteration();
    (void)input_monitor_task_watchdog_reset(watchdog_registered);
    (void)wait_ms;

    return ESP_OK;
}

void input_monitor_reset_for_testing(void)
{
    memset(&s_state, 0, sizeof(s_state));
    s_button_irq_pending = false;
    s_lock = NULL;
    memset(&s_lock_buffer, 0, sizeof(s_lock_buffer));
}
#endif

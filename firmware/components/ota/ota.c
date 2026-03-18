#include "ota.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "ota";

#define OTA_REBOOT_TASK_STACK_WORDS 3072U
#define OTA_REBOOT_TASK_PRIORITY 5U

typedef struct {
    SemaphoreHandle_t lock;
    StaticSemaphore_t lock_buffer;
    bool update_in_progress;
    bool reboot_scheduled;
    esp_ota_handle_t handle;
    const esp_partition_t *partition;
    size_t bytes_written;
} ota_state_t;

static ota_state_t s_state;

static esp_err_t ota_ensure_lock(void)
{
    if (s_state.lock != NULL) {
        return ESP_OK;
    }

    s_state.lock = xSemaphoreCreateMutexStatic(&s_state.lock_buffer);
    return (s_state.lock != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

static bool ota_lock(void)
{
    return (ota_ensure_lock() == ESP_OK) && (xSemaphoreTake(s_state.lock, portMAX_DELAY) == pdTRUE);
}

static void ota_unlock(void)
{
    if (s_state.lock != NULL) {
        xSemaphoreGive(s_state.lock);
    }
}

static void ota_clear_update_state(void)
{
    s_state.update_in_progress = false;
    s_state.handle = 0U;
    s_state.partition = NULL;
    s_state.bytes_written = 0U;
}

#if !defined(OTA_ENABLE_TESTING_API)
static void ota_reboot_task(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(OTA_REBOOT_DELAY_MS));
    esp_restart();
}
#endif

esp_err_t ota_begin_update(size_t image_size, ota_target_info_t *out_target)
{
    const esp_partition_t *partition = NULL;
    esp_ota_handle_t handle = 0U;
    esp_err_t err;

    if (image_size == 0U) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (!ota_lock()) {
        return ESP_ERR_NO_MEM;
    }

    if (s_state.update_in_progress || s_state.reboot_scheduled) {
        ota_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    partition = esp_ota_get_next_update_partition(NULL);
    if (partition == NULL) {
        ota_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    if (image_size > partition->size) {
        ota_unlock();
        return ESP_ERR_INVALID_SIZE;
    }

    err = esp_ota_begin(partition, image_size, &handle);
    if (err != ESP_OK) {
        ota_unlock();
        return err;
    }

    s_state.update_in_progress = true;
    s_state.handle = handle;
    s_state.partition = partition;
    s_state.bytes_written = 0U;

    if (out_target != NULL) {
        memset(out_target, 0, sizeof(*out_target));
        strncpy(out_target->partition_label, partition->label, sizeof(out_target->partition_label) - 1U);
        out_target->partition_size = partition->size;
    }

    ota_unlock();
    return ESP_OK;
}

esp_err_t ota_write_chunk(const void *data, size_t size)
{
    esp_err_t err;

    if ((data == NULL) && (size > 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (size == 0U) {
        return ESP_OK;
    }

    if (!ota_lock()) {
        return ESP_ERR_NO_MEM;
    }

    if (!s_state.update_in_progress) {
        ota_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_ota_write(s_state.handle, data, size);
    if (err == ESP_OK) {
        s_state.bytes_written += size;
    }

    ota_unlock();
    return err;
}

esp_err_t ota_finalize_update(void)
{
    esp_err_t err;

    if (!ota_lock()) {
        return ESP_ERR_NO_MEM;
    }

    if (!s_state.update_in_progress || (s_state.partition == NULL)) {
        ota_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_ota_end(s_state.handle);
    if (err != ESP_OK) {
        ota_clear_update_state();
        ota_unlock();
        return err;
    }

    err = esp_ota_set_boot_partition(s_state.partition);
    ota_clear_update_state();
    ota_unlock();
    return err;
}

void ota_abort_update(void)
{
    if (!ota_lock()) {
        return;
    }

    if (s_state.update_in_progress) {
        (void)esp_ota_abort(s_state.handle);
        ota_clear_update_state();
    }

    ota_unlock();
}

esp_err_t ota_schedule_reboot(void)
{
#if defined(OTA_ENABLE_TESTING_API)
    if (!ota_lock()) {
        return ESP_ERR_NO_MEM;
    }

    if (s_state.reboot_scheduled) {
        ota_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    s_state.reboot_scheduled = true;
    ota_unlock();
    return ESP_OK;
#else
    TaskHandle_t task_handle = NULL;
    BaseType_t task_result;

    if (!ota_lock()) {
        return ESP_ERR_NO_MEM;
    }

    if (s_state.reboot_scheduled) {
        ota_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    task_result = xTaskCreate(ota_reboot_task,
                              "ota_reboot",
                              OTA_REBOOT_TASK_STACK_WORDS,
                              NULL,
                              OTA_REBOOT_TASK_PRIORITY,
                              &task_handle);
    if (task_result != pdPASS) {
        s_state.reboot_scheduled = true;
        ota_unlock();
        ESP_LOGW(TAG, "Falling back to an immediate restart because reboot task creation failed");
        esp_restart();
        return ESP_OK;
    }

    s_state.reboot_scheduled = true;
    ota_unlock();
    return ESP_OK;
#endif
}

esp_err_t ota_confirm_running_image_if_pending(void)
{
    const esp_partition_t *running_partition = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
    esp_err_t err;

    if (running_partition == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    err = esp_ota_get_state_partition(running_partition, &ota_state);
    if ((err == ESP_ERR_NOT_SUPPORTED) || (err == ESP_ERR_NOT_FOUND)) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (ota_state != ESP_OTA_IMG_PENDING_VERIFY) {
        return ESP_OK;
    }

    err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Confirmed pending OTA image as valid");
    }
    return err;
}

#if defined(UNIT_TEST) || defined(OTA_ENABLE_TESTING_API)
void ota_reset_for_testing(void)
{
    memset(&s_state, 0, sizeof(s_state));
}

bool ota_reboot_scheduled_for_testing(void)
{
    return s_state.reboot_scheduled;
}
#endif

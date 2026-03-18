#include "esp_ota_stubs.h"

#include <string.h>

typedef struct {
    const esp_partition_t *next_update_partition;
    const esp_partition_t *running_partition;
    const esp_partition_t *last_begin_partition;
    const esp_partition_t *last_boot_partition;
    esp_err_t begin_result;
    esp_err_t write_result;
    esp_err_t end_result;
    esp_err_t boot_partition_result;
    esp_err_t state_result;
    esp_err_t mark_valid_result;
    esp_ota_img_states_t running_partition_state;
    size_t begin_count;
    size_t write_count;
    size_t abort_count;
    size_t mark_valid_count;
    size_t total_bytes_written;
    size_t last_begin_size;
    esp_ota_handle_t last_handle;
} esp_ota_stub_state_t;

static esp_ota_stub_state_t s_state;

void esp_ota_stub_reset(void)
{
    memset(&s_state, 0, sizeof(s_state));
    s_state.begin_result = ESP_OK;
    s_state.write_result = ESP_OK;
    s_state.end_result = ESP_OK;
    s_state.boot_partition_result = ESP_OK;
    s_state.state_result = ESP_ERR_NOT_SUPPORTED;
    s_state.mark_valid_result = ESP_OK;
    s_state.running_partition_state = ESP_OTA_IMG_VALID;
    s_state.last_handle = 1U;
}

void esp_ota_stub_set_next_update_partition(const esp_partition_t *partition)
{
    s_state.next_update_partition = partition;
}

void esp_ota_stub_set_running_partition(const esp_partition_t *partition)
{
    s_state.running_partition = partition;
}

void esp_ota_stub_set_begin_result(esp_err_t result)
{
    s_state.begin_result = result;
}

void esp_ota_stub_set_write_result(esp_err_t result)
{
    s_state.write_result = result;
}

void esp_ota_stub_set_end_result(esp_err_t result)
{
    s_state.end_result = result;
}

void esp_ota_stub_set_boot_partition_result(esp_err_t result)
{
    s_state.boot_partition_result = result;
}

void esp_ota_stub_set_state_result(esp_err_t result)
{
    s_state.state_result = result;
}

void esp_ota_stub_set_running_partition_state(esp_ota_img_states_t state)
{
    s_state.running_partition_state = state;
}

void esp_ota_stub_set_mark_valid_result(esp_err_t result)
{
    s_state.mark_valid_result = result;
}

size_t esp_ota_stub_get_begin_count(void)
{
    return s_state.begin_count;
}

size_t esp_ota_stub_get_write_count(void)
{
    return s_state.write_count;
}

size_t esp_ota_stub_get_abort_count(void)
{
    return s_state.abort_count;
}

size_t esp_ota_stub_get_mark_valid_count(void)
{
    return s_state.mark_valid_count;
}

size_t esp_ota_stub_get_total_bytes_written(void)
{
    return s_state.total_bytes_written;
}

size_t esp_ota_stub_get_last_begin_size(void)
{
    return s_state.last_begin_size;
}

const esp_partition_t *esp_ota_stub_get_last_begin_partition(void)
{
    return s_state.last_begin_partition;
}

const esp_partition_t *esp_ota_stub_get_last_boot_partition(void)
{
    return s_state.last_boot_partition;
}

const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *start_from)
{
    (void)start_from;
    return s_state.next_update_partition;
}

esp_err_t esp_ota_begin(const esp_partition_t *partition, size_t image_size, esp_ota_handle_t *out_handle)
{
    s_state.last_begin_partition = partition;
    s_state.last_begin_size = image_size;
    ++s_state.begin_count;

    if (s_state.begin_result != ESP_OK) {
        if (out_handle != NULL) {
            *out_handle = 0U;
        }
        return s_state.begin_result;
    }

    if (out_handle != NULL) {
        *out_handle = s_state.last_handle;
    }
    return ESP_OK;
}

esp_err_t esp_ota_write(esp_ota_handle_t handle, const void *data, size_t size)
{
    (void)data;

    ++s_state.write_count;
    if ((s_state.write_result == ESP_OK) && (handle == s_state.last_handle)) {
        s_state.total_bytes_written += size;
    }
    return s_state.write_result;
}

esp_err_t esp_ota_end(esp_ota_handle_t handle)
{
    return (handle == s_state.last_handle) ? s_state.end_result : ESP_ERR_INVALID_ARG;
}

esp_err_t esp_ota_abort(esp_ota_handle_t handle)
{
    if (handle != s_state.last_handle) {
        return ESP_ERR_INVALID_ARG;
    }

    ++s_state.abort_count;
    return ESP_OK;
}

esp_err_t esp_ota_set_boot_partition(const esp_partition_t *partition)
{
    s_state.last_boot_partition = partition;
    return s_state.boot_partition_result;
}

const esp_partition_t *esp_ota_get_running_partition(void)
{
    return s_state.running_partition;
}

esp_err_t esp_ota_get_state_partition(const esp_partition_t *partition, esp_ota_img_states_t *ota_state)
{
    if ((partition == NULL) || (ota_state == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_state.state_result == ESP_OK) {
        *ota_state = s_state.running_partition_state;
    }
    return s_state.state_result;
}

esp_err_t esp_ota_mark_app_valid_cancel_rollback(void)
{
    ++s_state.mark_valid_count;
    return s_state.mark_valid_result;
}

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_partition.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t esp_ota_handle_t;

typedef enum {
    ESP_OTA_IMG_UNDEFINED = 0,
    ESP_OTA_IMG_PENDING_VERIFY = 1,
    ESP_OTA_IMG_VALID = 2,
    ESP_OTA_IMG_INVALID = 3,
    ESP_OTA_IMG_ABORTED = 4,
} esp_ota_img_states_t;

const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *start_from);
esp_err_t esp_ota_begin(const esp_partition_t *partition, size_t image_size, esp_ota_handle_t *out_handle);
esp_err_t esp_ota_write(esp_ota_handle_t handle, const void *data, size_t size);
esp_err_t esp_ota_end(esp_ota_handle_t handle);
esp_err_t esp_ota_abort(esp_ota_handle_t handle);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *partition);
const esp_partition_t *esp_ota_get_running_partition(void);
esp_err_t esp_ota_get_state_partition(const esp_partition_t *partition, esp_ota_img_states_t *ota_state);
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void);

#ifdef __cplusplus
}
#endif

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_PARTITION_LABEL_MAX_LEN 16U
#define OTA_REBOOT_DELAY_MS 2000U

typedef struct {
    char partition_label[OTA_PARTITION_LABEL_MAX_LEN + 1U];
    size_t partition_size;
} ota_target_info_t;

esp_err_t ota_begin_update(size_t image_size, ota_target_info_t *out_target);
esp_err_t ota_write_chunk(const void *data, size_t size);
esp_err_t ota_finalize_update(void);
void ota_abort_update(void);
esp_err_t ota_schedule_reboot(void);
esp_err_t ota_confirm_running_image_if_pending(void);

#if defined(UNIT_TEST) || defined(OTA_ENABLE_TESTING_API)
void ota_reset_for_testing(void);
bool ota_reboot_scheduled_for_testing(void);
#endif

#ifdef __cplusplus
}
#endif

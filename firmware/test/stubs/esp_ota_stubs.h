#pragma once

#include <stddef.h>

#include "esp_ota_ops.h"

void esp_ota_stub_reset(void);
void esp_ota_stub_set_next_update_partition(const esp_partition_t *partition);
void esp_ota_stub_set_running_partition(const esp_partition_t *partition);
void esp_ota_stub_set_begin_result(esp_err_t result);
void esp_ota_stub_set_write_result(esp_err_t result);
void esp_ota_stub_set_end_result(esp_err_t result);
void esp_ota_stub_set_boot_partition_result(esp_err_t result);
void esp_ota_stub_set_state_result(esp_err_t result);
void esp_ota_stub_set_running_partition_state(esp_ota_img_states_t state);
void esp_ota_stub_set_mark_valid_result(esp_err_t result);
size_t esp_ota_stub_get_begin_count(void);
size_t esp_ota_stub_get_write_count(void);
size_t esp_ota_stub_get_abort_count(void);
size_t esp_ota_stub_get_mark_valid_count(void);
size_t esp_ota_stub_get_total_bytes_written(void);
size_t esp_ota_stub_get_last_begin_size(void);
const esp_partition_t *esp_ota_stub_get_last_begin_partition(void);
const esp_partition_t *esp_ota_stub_get_last_boot_partition(void);

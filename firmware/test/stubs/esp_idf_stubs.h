#pragma once

#include <stdarg.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    ESP_LOG_NONE = 0,
    ESP_LOG_ERROR,
    ESP_LOG_WARN,
    ESP_LOG_INFO,
    ESP_LOG_DEBUG,
    ESP_LOG_VERBOSE,
} esp_log_level_t;

const char *esp_err_to_name(esp_err_t err);
void esp_log_write(esp_log_level_t level, const char *tag, const char *format, ...);
int64_t esp_timer_get_time(void);
void esp_check_stub_log_error(const char *tag, esp_err_t err, const char *format, ...);

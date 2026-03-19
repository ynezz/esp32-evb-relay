#pragma once

#include "esp_idf_stubs.h"
#include "esp_log.h"

#define ESP_RETURN_ON_ERROR(expr, tag, format, ...)                                      \
    do {                                                                                 \
        esp_err_t esp_return_on_error_err_rc_ = (expr);                                  \
        if (esp_return_on_error_err_rc_ != ESP_OK) {                                     \
            esp_check_stub_log_error((tag), esp_return_on_error_err_rc_, (format),       \
                                     ##__VA_ARGS__);                                     \
            return esp_return_on_error_err_rc_;                                           \
        }                                                                                \
    } while (0)

#define ESP_RETURN_ON_FALSE(condition, err_code, tag, format, ...)                       \
    do {                                                                                 \
        if (!(condition)) {                                                              \
            esp_check_stub_log_error((tag), (err_code), (format), ##__VA_ARGS__);       \
            return (err_code);                                                           \
        }                                                                                \
    } while (0)

#define ESP_GOTO_ON_ERROR(expr, goto_label, tag, format, ...)                            \
    do {                                                                                 \
        esp_err_t esp_goto_on_error_err_rc_ = (expr);                                    \
        if (esp_goto_on_error_err_rc_ != ESP_OK) {                                       \
            esp_check_stub_log_error((tag), esp_goto_on_error_err_rc_, (format),         \
                                     ##__VA_ARGS__);                                     \
            ret = esp_goto_on_error_err_rc_;                                              \
            goto goto_label;                                                             \
        }                                                                                \
    } while (0)

#define ESP_GOTO_ON_FALSE(condition, err_code, goto_label, tag, format, ...)             \
    do {                                                                                 \
        if (!(condition)) {                                                              \
            esp_check_stub_log_error((tag), (err_code), (format), ##__VA_ARGS__);       \
            ret = (err_code);                                                            \
            goto goto_label;                                                             \
        }                                                                                \
    } while (0)

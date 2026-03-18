#include "esp_idf_stubs.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *esp_stub_log_level_name(esp_log_level_t level)
{
    switch (level) {
    case ESP_LOG_ERROR:
        return "E";
    case ESP_LOG_WARN:
        return "W";
    case ESP_LOG_INFO:
        return "I";
    case ESP_LOG_DEBUG:
        return "D";
    case ESP_LOG_VERBOSE:
        return "V";
    default:
        return "?";
    }
}

static bool s_time_override_enabled;
static int64_t s_time_override_us;
static size_t s_restart_count;

const char *esp_err_to_name(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return "ESP_OK";
    case ESP_FAIL:
        return "ESP_FAIL";
    case ESP_ERR_NO_MEM:
        return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG:
        return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE:
        return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_TIMEOUT:
        return "ESP_ERR_TIMEOUT";
    case ESP_ERR_NOT_FOUND:
        return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_INVALID_SIZE:
        return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_SUPPORTED:
        return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_OTA_VALIDATE_FAILED:
        return "ESP_ERR_OTA_VALIDATE_FAILED";
    case ESP_ERR_OTA_ROLLBACK_INVALID_STATE:
        return "ESP_ERR_OTA_ROLLBACK_INVALID_STATE";
    case ESP_ERR_NVS_NOT_FOUND:
        return "ESP_ERR_NVS_NOT_FOUND";
    case ESP_ERR_NVS_TYPE_MISMATCH:
        return "ESP_ERR_NVS_TYPE_MISMATCH";
    case ESP_ERR_NVS_INVALID_LENGTH:
        return "ESP_ERR_NVS_INVALID_LENGTH";
    case ESP_ERR_NVS_NO_FREE_PAGES:
        return "ESP_ERR_NVS_NO_FREE_PAGES";
    case ESP_ERR_NVS_NEW_VERSION_FOUND:
        return "ESP_ERR_NVS_NEW_VERSION_FOUND";
    default:
        return "ESP_ERR_UNKNOWN";
    }
}

void esp_log_write(esp_log_level_t level, const char *tag, const char *format, ...)
{
    va_list args;

    fprintf(stderr, "%s (%s) ", esp_stub_log_level_name(level), (tag != NULL) ? tag : "-");

    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);

    fputc('\n', stderr);
}

int64_t esp_timer_get_time(void)
{
    if (s_time_override_enabled) {
        return s_time_override_us;
    }

    struct timespec now = {0};

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }

    return ((int64_t)now.tv_sec * 1000000LL) + (now.tv_nsec / 1000LL);
}

void esp_check_stub_log_error(const char *tag, esp_err_t err, const char *format, ...)
{
    char message[512];
    va_list args;

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    fprintf(stderr, "E (%s) %s: %s\n", (tag != NULL) ? tag : "-", message, esp_err_to_name(err));
}

void esp_stub_reset_time_override(void)
{
    s_time_override_enabled = false;
    s_time_override_us = 0;
}

void esp_stub_set_time_us(int64_t time_us)
{
    s_time_override_enabled = true;
    s_time_override_us = time_us;
}

void esp_stub_advance_time_us(int64_t delta_us)
{
    s_time_override_enabled = true;
    s_time_override_us += delta_us;
}

void esp_stub_reset_restart_count(void)
{
    s_restart_count = 0U;
}

size_t esp_stub_get_restart_count(void)
{
    return s_restart_count;
}

void esp_restart(void)
{
    ++s_restart_count;
}

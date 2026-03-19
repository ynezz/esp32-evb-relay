#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    int dummy;
} wifi_init_config_t;

typedef enum {
    WIFI_STORAGE_RAM = 0,
} wifi_storage_t;

typedef enum {
    WIFI_MODE_STA = 1,
} wifi_mode_t;

typedef enum {
    WIFI_AUTH_OPEN = 0,
    WIFI_AUTH_WPA2_PSK = 1,
} wifi_auth_mode_t;

typedef struct {
    uint8_t authmode;
} wifi_scan_threshold_t;

typedef struct {
    bool capable;
    bool required;
} wifi_pmf_config_t;

typedef struct {
    uint8_t ssid[32];
    uint8_t password[64];
    wifi_scan_threshold_t threshold;
    wifi_pmf_config_t pmf_cfg;
} wifi_sta_config_t;

typedef struct {
    wifi_sta_config_t sta;
} wifi_config_t;

typedef struct {
    uint8_t reason;
} wifi_event_sta_disconnected_t;

#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){0})
#define WIFI_IF_STA 0

#define WIFI_EVENT_STA_START 1
#define WIFI_EVENT_STA_CONNECTED 2
#define WIFI_EVENT_STA_DISCONNECTED 3
#define WIFI_EVENT_STA_STOP 4

esp_err_t esp_wifi_init(const wifi_init_config_t *config);
esp_err_t esp_wifi_set_storage(wifi_storage_t storage);
esp_err_t esp_wifi_set_mode(wifi_mode_t mode);
esp_err_t esp_wifi_set_config(int interface, const wifi_config_t *config);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_stop(void);
esp_err_t esp_wifi_deinit(void);

#pragma once

#include <stdint.h>

#include "esp_http_server.h"

#define REST_API_HTTPD_STACK_SIZE 8192U

httpd_config_t rest_api_make_httpd_config(uint16_t server_port, uint16_t max_uri_handlers);

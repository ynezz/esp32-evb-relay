#pragma once

#include <stddef.h>

#include "esp_err.h"

typedef struct httpd_req httpd_req_t;
typedef void *httpd_handle_t;

#define HTTPD_SOCK_ERR_TIMEOUT -3

int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len);
size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *field);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r,
                                      const char *field,
                                      char *value,
                                      size_t value_len);

#pragma once

#include <stddef.h>

typedef struct httpd_req httpd_req_t;
typedef void *httpd_handle_t;

#define HTTPD_SOCK_ERR_TIMEOUT -3

int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len);

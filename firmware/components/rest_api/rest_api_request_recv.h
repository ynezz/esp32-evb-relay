#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t rest_api_request_recv_exact(httpd_req_t *req,
                                      char *buffer,
                                      size_t expected_len,
                                      size_t *out_received);

#ifdef __cplusplus
}
#endif

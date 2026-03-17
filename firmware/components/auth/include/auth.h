#pragma once

#include "esp_err.h"
#include "rest_api.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t auth_init(void);
rest_api_auth_result_t auth_check(httpd_req_t *req, void *ctx);

#ifdef __cplusplus
}
#endif

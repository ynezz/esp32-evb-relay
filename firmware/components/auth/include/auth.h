#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "rest_api.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t auth_init(void);
rest_api_auth_result_t auth_check(httpd_req_t *req, void *ctx);

#if defined(UNIT_TEST) || defined(AUTH_ENABLE_TESTING_API)
size_t auth_constant_time_compare_iterations_for_testing(const char *lhs, const char *rhs);
#endif

#ifdef __cplusplus
}
#endif

#include "auth.h"

#include <string.h>

#include "device_config.h"
#include "esp_http_server.h"
#include "unity.h"

typedef struct httpd_req {
    const char *authorization;
} httpd_req;

size_t httpd_req_get_hdr_value_len(httpd_req_t *req, const char *field)
{
    const httpd_req *mock_req = (const httpd_req *)req;

    TEST_ASSERT_NOT_NULL(mock_req);
    TEST_ASSERT_EQUAL_STRING("Authorization", field);
    return (mock_req->authorization != NULL) ? strlen(mock_req->authorization) : 0U;
}

esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *req, const char *field, char *buffer, size_t buffer_len)
{
    const httpd_req *mock_req = (const httpd_req *)req;
    size_t authorization_len;

    TEST_ASSERT_NOT_NULL(mock_req);
    TEST_ASSERT_EQUAL_STRING("Authorization", field);
    TEST_ASSERT_NOT_NULL(buffer);

    if (mock_req->authorization == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    authorization_len = strlen(mock_req->authorization);
    if (authorization_len >= buffer_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(buffer, mock_req->authorization, authorization_len + 1U);
    return ESP_OK;
}

static void fill_token(char *buffer, size_t length, char value)
{
    memset(buffer, value, length);
    buffer[length] = '\0';
}

static void test_auth_constant_time_compare_uses_fixed_iteration_bound(void)
{
    char short_token[] = "short";
    char max_token[DEVICE_CONFIG_API_TOKEN_MAX_LEN + 1U] = {0};

    fill_token(max_token, DEVICE_CONFIG_API_TOKEN_MAX_LEN, 'x');

    TEST_ASSERT_EQUAL_UINT32(DEVICE_CONFIG_API_TOKEN_MAX_LEN,
                             auth_constant_time_compare_iterations_for_testing(short_token, short_token));
    TEST_ASSERT_EQUAL_UINT32(DEVICE_CONFIG_API_TOKEN_MAX_LEN,
                             auth_constant_time_compare_iterations_for_testing(short_token, max_token));
    TEST_ASSERT_EQUAL_UINT32(DEVICE_CONFIG_API_TOKEN_MAX_LEN,
                             auth_constant_time_compare_iterations_for_testing(max_token, short_token));
}

void test_auth_suite(void)
{
    RUN_TEST(test_auth_constant_time_compare_uses_fixed_iteration_bound);
}

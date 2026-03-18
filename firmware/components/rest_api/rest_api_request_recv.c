#include "rest_api_request_recv.h"

#define REST_API_REQUEST_RECV_MAX_TIMEOUTS 3U

esp_err_t rest_api_request_recv_exact(httpd_req_t *req,
                                      char *buffer,
                                      size_t expected_len,
                                      size_t *out_received)
{
    size_t remaining = expected_len;
    size_t offset = 0U;
    size_t timeout_count = 0U;

    if (req == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if ((buffer == NULL) && (expected_len > 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    while (remaining > 0U) {
        int received = httpd_req_recv(req, buffer + offset, remaining);

        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            ++timeout_count;
            if (timeout_count >= REST_API_REQUEST_RECV_MAX_TIMEOUTS) {
                return ESP_ERR_TIMEOUT;
            }

            continue;
        }

        if (received <= 0) {
            return ESP_FAIL;
        }

        timeout_count = 0U;
        remaining -= (size_t)received;
        offset += (size_t)received;
    }

    if (out_received != NULL) {
        *out_received = offset;
    }

    return ESP_OK;
}

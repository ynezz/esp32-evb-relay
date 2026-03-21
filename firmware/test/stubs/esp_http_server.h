#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct httpd_req httpd_req_t;
typedef void *httpd_handle_t;
typedef bool (*httpd_uri_match_func_t)(const char *reference_uri,
                                       const char *uri_to_match,
                                       size_t match_upto);

typedef struct httpd_config {
    unsigned task_priority;
    size_t stack_size;
    int core_id;
    uint32_t task_caps;
    uint16_t server_port;
    uint16_t ctrl_port;
    uint16_t max_open_sockets;
    uint16_t max_uri_handlers;
    uint16_t max_resp_headers;
    uint16_t backlog_conn;
    bool lru_purge_enable;
    uint16_t recv_wait_timeout;
    uint16_t send_wait_timeout;
    void *global_user_ctx;
    void *global_user_ctx_free_fn;
    void *global_transport_ctx;
    void *global_transport_ctx_free_fn;
    bool enable_so_linger;
    int linger_timeout;
    bool keep_alive_enable;
    int keep_alive_idle;
    int keep_alive_interval;
    int keep_alive_count;
    void *open_fn;
    void *close_fn;
    httpd_uri_match_func_t uri_match_fn;
} httpd_config_t;

#define HTTPD_DEFAULT_CONFIG() {  \
    .stack_size = 4096U,          \
    .server_port = 80U,           \
    .max_uri_handlers = 8U,       \
    .uri_match_fn = NULL,         \
}

#define HTTPD_SOCK_ERR_TIMEOUT -3

bool httpd_uri_match_wildcard(const char *reference_uri,
                              const char *uri_to_match,
                              size_t match_upto);
int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len);
size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *field);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r,
                                      const char *field,
                                      char *value,
                                      size_t value_len);

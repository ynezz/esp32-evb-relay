#pragma once

#include <stdbool.h>

#include "esp_http_server.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "rest_api.h"

#define REST_API_SSE_INVALID_SOCKFD (-1)

typedef struct {
    bool active;
    bool close_requested;
    QueueHandle_t queue;
    TaskHandle_t task_handle;
    httpd_req_t *req;
    rest_api_status_view_t status;
    int sockfd;
} rest_api_sse_client_t;

typedef struct {
    bool (*lock)(void);
    void (*unlock)(void);
    void (*delete_queue)(QueueHandle_t queue);
    void (*send_terminal_chunk)(httpd_req_t *req);
    void (*complete_async_request)(httpd_req_t *req);
    void (*delete_task)(TaskHandle_t task_handle);
    TaskHandle_t (*current_task_handle)(void);
} rest_api_sse_lifetime_hooks_t;

void rest_api_sse_reset_client(rest_api_sse_client_t *client);
void rest_api_sse_release_startup_client_lifetime(rest_api_sse_client_t *client,
                                                  httpd_req_t *req,
                                                  const rest_api_sse_lifetime_hooks_t *hooks);
void rest_api_sse_release_client_lifetime(rest_api_sse_client_t *client,
                                          const rest_api_sse_lifetime_hooks_t *hooks);
void rest_api_sse_force_release_client_lifetime(rest_api_sse_client_t *client,
                                                const rest_api_sse_lifetime_hooks_t *hooks);

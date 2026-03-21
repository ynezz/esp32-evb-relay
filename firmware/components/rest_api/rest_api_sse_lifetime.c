#include "rest_api_sse_lifetime.h"

#include <string.h>

void rest_api_sse_reset_client(rest_api_sse_client_t *client)
{
    if (client == NULL) {
        return;
    }

    memset(client, 0, sizeof(*client));
    client->sockfd = REST_API_SSE_INVALID_SOCKFD;
}

static bool rest_api_sse_lifetime_lock(const rest_api_sse_lifetime_hooks_t *hooks)
{
    return (hooks != NULL) && (hooks->lock != NULL) && hooks->lock();
}

static void rest_api_sse_lifetime_unlock(const rest_api_sse_lifetime_hooks_t *hooks, bool locked)
{
    if (locked && (hooks != NULL) && (hooks->unlock != NULL)) {
        hooks->unlock();
    }
}

static void rest_api_sse_lifetime_clear_client(rest_api_sse_client_t *client,
                                               const rest_api_sse_lifetime_hooks_t *hooks)
{
    bool locked;

    if (client == NULL) {
        return;
    }

    locked = rest_api_sse_lifetime_lock(hooks);
    rest_api_sse_reset_client(client);
    rest_api_sse_lifetime_unlock(hooks, locked);
}

void rest_api_sse_release_startup_client_lifetime(rest_api_sse_client_t *client,
                                                  httpd_req_t *req,
                                                  const rest_api_sse_lifetime_hooks_t *hooks)
{
    QueueHandle_t queue = NULL;
    httpd_req_t *owned_req = NULL;
    bool locked;

    if ((client == NULL) || (hooks == NULL)) {
        return;
    }

    locked = rest_api_sse_lifetime_lock(hooks);
    queue = client->queue;
    owned_req = (client->req != NULL) ? client->req : req;
    client->queue = NULL;
    client->req = NULL;
    rest_api_sse_lifetime_unlock(hooks, locked);

    if ((queue != NULL) && (hooks->delete_queue != NULL)) {
        hooks->delete_queue(queue);
    }

    /* Startup failures never hand ownership to a client task, but the slot
     * must stay active until async completion returns so stop-side polling
     * cannot observe a cleared slot while ESP-IDF still owns the request. */
    if ((owned_req != NULL) && (hooks->complete_async_request != NULL)) {
        hooks->complete_async_request(owned_req);
    }

    rest_api_sse_lifetime_clear_client(client, hooks);
}

void rest_api_sse_release_client_lifetime(rest_api_sse_client_t *client,
                                          const rest_api_sse_lifetime_hooks_t *hooks)
{
    QueueHandle_t queue = NULL;
    httpd_req_t *req = NULL;
    bool locked;

    if ((client == NULL) || (hooks == NULL)) {
        return;
    }

    locked = rest_api_sse_lifetime_lock(hooks);
    queue = client->queue;
    req = client->req;
    client->queue = NULL;
    client->req = NULL;
    rest_api_sse_lifetime_unlock(hooks, locked);

    if ((queue != NULL) && (hooks->delete_queue != NULL)) {
        hooks->delete_queue(queue);
    }

    if (req != NULL) {
        if (hooks->send_terminal_chunk != NULL) {
            hooks->send_terminal_chunk(req);
        }

        /* Keep the slot active until async completion returns so
         * rest_api_sse_stop() never treats a cleared slot as proof that the
         * HTTP server has finished relinquishing the async request. */
        if (hooks->complete_async_request != NULL) {
            hooks->complete_async_request(req);
        }
    }

    rest_api_sse_lifetime_clear_client(client, hooks);
}

void rest_api_sse_force_release_client_lifetime(rest_api_sse_client_t *client,
                                                const rest_api_sse_lifetime_hooks_t *hooks)
{
    QueueHandle_t queue = NULL;
    httpd_req_t *req = NULL;
    TaskHandle_t task_handle = NULL;
    TaskHandle_t current_task = NULL;
    bool locked;

    if ((client == NULL) || (hooks == NULL)) {
        return;
    }

    if (hooks->current_task_handle != NULL) {
        current_task = hooks->current_task_handle();
    }

    locked = rest_api_sse_lifetime_lock(hooks);
    if (!client->active) {
        rest_api_sse_lifetime_unlock(hooks, locked);
        return;
    }

    queue = client->queue;
    req = client->req;
    task_handle = client->task_handle;

    /* Delete a foreign task before clearing any fields it could still touch. */
    if ((task_handle != NULL) && (task_handle != current_task) && (hooks->delete_task != NULL)) {
        hooks->delete_task(task_handle);
    }

    client->queue = NULL;
    client->req = NULL;
    client->task_handle = NULL;
    rest_api_sse_lifetime_unlock(hooks, locked);

    if ((queue != NULL) && (hooks->delete_queue != NULL)) {
        hooks->delete_queue(queue);
    }

    if ((req != NULL) && (hooks->complete_async_request != NULL)) {
        hooks->complete_async_request(req);
    }

    rest_api_sse_lifetime_clear_client(client, hooks);
}

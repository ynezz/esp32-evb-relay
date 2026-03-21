#include "rest_api_server_config.h"

#include "rest_api.h"

httpd_config_t rest_api_make_httpd_config(uint16_t server_port, uint16_t max_uri_handlers)
{
    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();

    server_config.server_port = (server_port != 0U) ? server_port : REST_API_DEFAULT_PORT;
    /* Config and OTA PUT handlers use sizeable request buffers, so the
     * server task needs more headroom than ESP-IDF's 4 KB default. */
    server_config.stack_size = REST_API_HTTPD_STACK_SIZE;
    /* SSE and long-lived REST clients can leave persistent sessions idle
     * for a short period after async completion. Let httpd reclaim the
     * least-recently-used session instead of starving new connections. */
    server_config.lru_purge_enable = true;
    server_config.max_uri_handlers = max_uri_handlers;
    server_config.uri_match_fn = httpd_uri_match_wildcard;
    return server_config;
}

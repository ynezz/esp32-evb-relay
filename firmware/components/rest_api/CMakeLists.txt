idf_component_register(SRCS "rest_api.c" "rest_api_request_recv.c" "rest_api_sse_lifetime.c"
                            "rest_api_wifi_config_update.c"
                       INCLUDE_DIRS "include"
                       REQUIRES app_update device_config esp_app_format esp_event esp_http_server
                                esp_system esp_timer freertos lwip
                                heap input_monitor json mod_io ota relay relay_events)

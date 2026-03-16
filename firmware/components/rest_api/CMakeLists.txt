idf_component_register(SRCS "rest_api.c"
                       INCLUDE_DIRS "include"
                       REQUIRES device_config esp_app_format esp_event esp_http_server esp_timer heap json)

idf_component_register(SRCS "auth.c"
                       INCLUDE_DIRS "include"
                       REQUIRES device_config esp_http_server esp_system log rest_api)

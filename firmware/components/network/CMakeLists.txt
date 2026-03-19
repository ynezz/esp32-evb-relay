idf_component_register(SRCS "network.c"
                       INCLUDE_DIRS "include"
                       REQUIRES board device_config esp_app_format esp_eth esp_event esp_netif esp_wifi freertos mdns)

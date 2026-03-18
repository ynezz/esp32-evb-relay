idf_component_register(SRCS "relay.c"
                       INCLUDE_DIRS "include"
                       REQUIRES board esp_driver_gpio esp_event esp_timer freertos relay_events)

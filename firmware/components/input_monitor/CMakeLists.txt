idf_component_register(SRCS "input_monitor.c"
                       INCLUDE_DIRS "include"
                       REQUIRES board device_config esp_driver_gpio esp_system esp_timer freertos
                                mod_io relay_events)

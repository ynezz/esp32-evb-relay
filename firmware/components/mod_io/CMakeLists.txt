idf_component_register(SRCS "mod_io.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_i2c esp_timer freertos)

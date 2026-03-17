idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES board device_config mod_io relay rest_api)

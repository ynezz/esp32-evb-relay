idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES board device_config mod_io rest_api)

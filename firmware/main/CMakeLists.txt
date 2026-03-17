idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES auth board device_config mod_io relay rest_api)

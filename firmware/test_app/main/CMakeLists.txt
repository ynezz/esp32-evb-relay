idf_component_register(SRC_DIRS "."
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES auth board device_config esp_netif lwip mod_io relay rest_api unity)

target_compile_definitions(${COMPONENT_LIB} PRIVATE DEVICE_CONFIG_ENABLE_TESTING_API=1)

idf_component_get_property(device_config_lib device_config COMPONENT_LIB)
target_compile_definitions(${device_config_lib} PRIVATE DEVICE_CONFIG_ENABLE_TESTING_API=1)

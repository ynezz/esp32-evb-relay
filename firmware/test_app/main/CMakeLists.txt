idf_component_register(SRC_DIRS "."
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES auth board device_config esp_netif input_monitor lwip mod_io relay
                                     relay_events rest_api unity)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
                           DEVICE_CONFIG_ENABLE_TESTING_API=1
                           INPUT_MONITOR_ENABLE_TESTING_API=1)

idf_component_get_property(device_config_lib device_config COMPONENT_LIB)
target_compile_definitions(${device_config_lib} PRIVATE DEVICE_CONFIG_ENABLE_TESTING_API=1)

idf_component_get_property(input_monitor_lib input_monitor COMPONENT_LIB)
target_compile_definitions(${input_monitor_lib} PRIVATE INPUT_MONITOR_ENABLE_TESTING_API=1)

cmake_minimum_required(VERSION 3.16)

project(evb_relay_host_tests LANGUAGES C)

include(CTest)
enable_testing()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT DEFINED ENV{IDF_PATH} OR "$ENV{IDF_PATH}" STREQUAL "")
    message(FATAL_ERROR "IDF_PATH must be set to an ESP-IDF checkout")
endif()

set(TEST_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(FIRMWARE_DIR "${TEST_DIR}/..")
set(STUBS_DIR "${TEST_DIR}/stubs")
set(UNITY_DIR "$ENV{IDF_PATH}/components/unity/unity")
set(UNITY_SRC "${UNITY_DIR}/src/unity.c")

if(NOT EXISTS "${UNITY_SRC}")
    message(FATAL_ERROR "Unity source not found at ${UNITY_SRC}")
endif()

add_library(unity_host STATIC "${UNITY_SRC}")
target_include_directories(unity_host PUBLIC "${UNITY_DIR}/src")

add_executable(host_tests
    "${TEST_DIR}/test_main.c"
    "${TEST_DIR}/device_config/test_device_config.c"
    "${TEST_DIR}/mod_io/test_mod_io.c"
    "${TEST_DIR}/relay/test_relay.c"
    "${TEST_DIR}/stubs/esp_idf_stubs.c"
    "${TEST_DIR}/stubs/freertos_stubs.c"
    "${TEST_DIR}/stubs/gpio_stubs.c"
    "${TEST_DIR}/stubs/i2c_stubs.c"
    "${TEST_DIR}/stubs/nvs_stubs.c"
    "${TEST_DIR}/stubs/esp_event_stubs.c"
    "${FIRMWARE_DIR}/components/board/board.c"
    "${FIRMWARE_DIR}/components/relay/relay.c"
    "${FIRMWARE_DIR}/components/mod_io/mod_io.c"
    "${FIRMWARE_DIR}/components/device_config/device_config.c"
)

target_include_directories(host_tests PRIVATE
    "${STUBS_DIR}"
    "${FIRMWARE_DIR}/components/board/include"
    "${FIRMWARE_DIR}/components/relay/include"
    "${FIRMWARE_DIR}/components/mod_io/include"
    "${FIRMWARE_DIR}/components/device_config/include"
    "${FIRMWARE_DIR}/components/rest_api/include"
)

target_compile_definitions(host_tests PRIVATE
    UNIT_TEST=1
    _POSIX_C_SOURCE=200809L
)

target_compile_options(host_tests PRIVATE
    -Wall
    -Wextra
)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(host_tests PRIVATE
        -fno-omit-frame-pointer
        -fsanitize=address
        -fsanitize=undefined
    )
    target_link_options(host_tests PRIVATE
        -fsanitize=address
        -fsanitize=undefined
    )
endif()

target_link_libraries(host_tests PRIVATE unity_host)

add_test(NAME host_smoke COMMAND host_tests)

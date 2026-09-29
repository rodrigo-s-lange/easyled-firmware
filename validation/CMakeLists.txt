cmake_minimum_required(VERSION 3.22)
get_filename_component(FRAMEWORK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
set(ESP_PIXELS_COMPOSER_DIR "${FRAMEWORK_ROOT}/../esp_pixels_composer")
set(ESP_PIXELS_FX_DIR "${FRAMEWORK_ROOT}/../esp_pixels_fx")
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/pixels_network"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/pixels_firmware_contract"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/pixels_payload_adapter"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/pixels_command_executor"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/pixels_durable_storage"
    "${FRAMEWORK_ROOT}/framework/core/esp32libfun_at"
    "${FRAMEWORK_ROOT}/framework/core/esp32libfun_rmt"
    "${FRAMEWORK_ROOT}/framework/core/esp32libfun_serial"
    "${FRAMEWORK_ROOT}/framework/core/esp32libfun_wifi_sta"
    "${FRAMEWORK_ROOT}/framework/libs/esp_pixels"
    "${ESP_PIXELS_FX_DIR}"
    "${ESP_PIXELS_COMPOSER_DIR}"
    "${ESP_PIXELS_COMPOSER_DIR}/adapter_esp_pixels"
)
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pixels_network_validation)

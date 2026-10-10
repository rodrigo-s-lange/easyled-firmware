cmake_minimum_required(VERSION 3.22)
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/dependencies.cmake")
list(APPEND EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/pixels_network"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/pixels_firmware_contract"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/pixels_payload_adapter"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/pixels_command_executor"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/pixels_durable_storage"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/pixels_control_runtime"
)
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(easyled_firmware_validation)

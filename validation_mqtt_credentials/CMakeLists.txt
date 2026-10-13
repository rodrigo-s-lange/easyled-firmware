cmake_minimum_required(VERSION 3.22)
# Lean target: only pixels_mqtt_credentials. No pixel/Composer/esp32libfun
# dependency at all -- this feature touches none of them, and the frozen
# `validation/` target already owns every check that does.
list(APPEND EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/pixels_mqtt_credentials"
)
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(easyled_mqtt_credentials_validation)

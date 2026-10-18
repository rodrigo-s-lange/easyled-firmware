cmake_minimum_required(VERSION 3.22)
# pixels_network itself depends on esp32libfun_wifi_sta (framework/core), so
# cmake/dependencies.cmake is unavoidable here -- unlike
# validation_mqtt_credentials/, which has no pixels_network dependency at
# all. `set(COMPONENTS main)` below still keeps the actual build lean: the
# pixel/Composer directories this file also registers are discoverable but
# never pulled into this target's dependency graph, since nothing in `main`
# REQUIRES them.
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/dependencies.cmake")
list(APPEND EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/pixels_network"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/pixels_mqtt_credentials"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/pixels_mqtt_session_adapter"
)
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(easyled_mqtt_session_validation)

cmake_minimum_required(VERSION 3.22)
# Lean target: pixels_bootstrap_identity only. No pixel/Composer/esp32libfun
# or pixels_network dependency at all -- this feature touches none of them.
list(APPEND EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/pixels_bootstrap_identity"
)
set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(easyled_bootid_validation)

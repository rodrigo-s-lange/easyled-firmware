set(EASYLED_DEPENDENCIES_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../dependencies"
    CACHE PATH "Directory containing EasyLED firmware dependencies")

set(ESP32LIBFUN_ROOT
    "${EASYLED_DEPENDENCIES_DIR}/esp32libfun"
    CACHE PATH "Pinned esp32libfun checkout")
set(ESP_PIXELS_FX_DIR
    "${EASYLED_DEPENDENCIES_DIR}/esp_pixels_fx"
    CACHE PATH "Pinned esp_pixels_fx checkout")
set(ESP_PIXELS_DIR
    "${EASYLED_DEPENDENCIES_DIR}/esp_pixels"
    CACHE PATH "Pinned esp_pixels checkout")
set(ESP_PIXELS_COMPOSER_DIR
    "${EASYLED_DEPENDENCIES_DIR}/esp_pixels_composer"
    CACHE PATH "Pinned esp_pixels_composer checkout")

foreach(required_path
        "${ESP32LIBFUN_ROOT}/framework/core/esp32libfun_at"
        "${ESP_PIXELS_DIR}/CMakeLists.txt"
        "${ESP_PIXELS_FX_DIR}/CMakeLists.txt"
        "${ESP_PIXELS_COMPOSER_DIR}/CMakeLists.txt")
    if(NOT EXISTS "${required_path}")
        message(FATAL_ERROR
            "Missing firmware dependency: ${required_path}. "
            "Run: git submodule update --init --recursive")
    endif()
endforeach()

list(APPEND EXTRA_COMPONENT_DIRS
    "${ESP32LIBFUN_ROOT}/framework/core/esp32libfun_at"
    "${ESP32LIBFUN_ROOT}/framework/core/esp32libfun_rmt"
    "${ESP32LIBFUN_ROOT}/framework/core/esp32libfun_serial"
    "${ESP32LIBFUN_ROOT}/framework/core/esp32libfun_wifi_sta"
    "${ESP_PIXELS_DIR}"
    "${ESP_PIXELS_FX_DIR}"
    "${ESP_PIXELS_COMPOSER_DIR}"
    "${ESP_PIXELS_COMPOSER_DIR}/adapter_esp_pixels")

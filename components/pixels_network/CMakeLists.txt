idf_component_register(SRCS "network_session.cpp" INCLUDE_DIRS "include"
    REQUIRES esp32libfun_wifi_sta mqtt esp_event freertos
    PRIV_REQUIRES esp_wifi esp_timer mbedtls)

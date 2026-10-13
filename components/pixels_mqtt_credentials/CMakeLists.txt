idf_component_register(SRCS "pixels_mqtt_credentials.cpp"
    INCLUDE_DIRS "include"
    REQUIRES cjson nvs_flash esp_http_client
    PRIV_REQUIRES esp_timer mbedtls)

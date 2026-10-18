idf_component_register(SRCS "main.cpp"
    REQUIRES pixels_network pixels_mqtt_credentials pixels_mqtt_session_adapter
             nvs_flash freertos mbedtls)

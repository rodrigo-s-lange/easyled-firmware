idf_component_register(SRCS "main.cpp"
    REQUIRES pixels_mqtt_credentials nvs_flash freertos mbedtls
    EMBED_TXTFILES
        "../fixtures/device-response.v1.json")

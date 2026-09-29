idf_component_register(SRCS "pixels_control_runtime.cpp"
    INCLUDE_DIRS "include"
    REQUIRES pixels_network pixels_firmware_contract pixels_payload_adapter
             pixels_command_executor pixels_durable_storage esp_pixels_composer
             esp_http_client mbedtls freertos)

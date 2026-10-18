idf_component_register(SRCS "main.cpp" INCLUDE_DIRS "."
    REQUIRES pixels_network pixels_firmware_contract pixels_payload_adapter
             pixels_command_executor pixels_durable_storage pixels_control_runtime
             pixels_mqtt_credentials pixels_mqtt_session_adapter pixels_bootstrap_identity
             esp_pixels esp_pixels_fx esp_pixels_composer adapter_esp_pixels
             esp_netif esp_timer heap nvs_flash log
    EMBED_TXTFILES
        "../validation/fixtures/config-payload-bench-gpio16-50.provisional.json"
        "../validation/fixtures/preset-payload.provisional.json")

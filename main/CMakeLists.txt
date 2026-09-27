idf_component_register(SRCS "main.cpp" INCLUDE_DIRS "."
    REQUIRES pixels_network pixels_firmware_contract esp_pixels esp_pixels_fx
             esp_pixels_composer adapter_esp_pixels esp_netif esp_timer nvs_flash log)

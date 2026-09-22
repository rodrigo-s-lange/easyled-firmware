idf_component_register(SRCS "main.cpp" INCLUDE_DIRS "."
    REQUIRES pixels_network esp_netif esp_timer nvs_flash log)

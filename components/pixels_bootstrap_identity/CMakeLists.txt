idf_component_register(SRCS "pixels_bootstrap_identity.cpp"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash
    PRIV_REQUIRES esp_hw_support)

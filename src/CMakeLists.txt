idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES driver esp_driver_gpio esp_driver_i2s esp_timer
)

idf_component_register(
    SRCS "test_display.c" "lodepng.c"
    INCLUDE_DIRS "."
    REQUIRES lilygo_epd47 esp_adc driver sdmmc esp_driver_gpio nvs_flash fatfs
)

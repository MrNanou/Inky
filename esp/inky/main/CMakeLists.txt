idf_component_register(
    SRCS "inky.c"
    INCLUDE_DIRS "."
    REQUIRES lilygo_epd47 miniz driver esp_adc sdmmc fatfs nvs_flash
)

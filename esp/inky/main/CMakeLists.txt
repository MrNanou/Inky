idf_component_register(
    SRCS "inky.c"
         "../../../shared/archive/archive_esp.c"
         "../../../shared/epub/epub.c"
         "../../../shared/layout/layout.c"
         "../../../shared/html/html.c"
    INCLUDE_DIRS "."
                 "../../../shared"
    REQUIRES lilygo_epd47 miniz driver esp_adc sdmmc fatfs nvs_flash
)

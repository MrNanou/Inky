idf_component_register(
    SRCS
        "src/ed047tc1.c"
        "src/epd_driver.c"
        "src/font.c"
        "src/i2s_data_bus.c"
        "src/rmt_pulse.c"
        "include/zlib/adler32.c"
        "include/zlib/compress.c"
        "include/zlib/crc32.c"
        "include/zlib/deflate.c"
        "include/zlib/gzclose.c"
        "include/zlib/gzlib.c"
        "include/zlib/gzread.c"
        "include/zlib/gzwrite.c"
        "include/zlib/infback.c"
        "include/zlib/inffast.c"
        "include/zlib/inflate.c"
        "include/zlib/inftrees.c"
        "include/zlib/trees.c"
        "include/zlib/uncompr.c"
        "include/zlib/zutil.c"
    INCLUDE_DIRS "include" "include/zlib"
    REQUIRES
        esp_driver_gpio
        esp_driver_i2s
        esp_driver_rmt
        esp_psram
        freertos
    PRIV_REQUIRES
        esp_lcd
)

# The bundled zlib uses old-style K&R function definitions.
# Disable the warning so the third-party code compiles on modern toolchains.
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-old-style-definition -Wno-deprecated-declarations -Wno-implicit-fallthrough)
endif()

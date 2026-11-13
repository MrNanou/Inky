#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_sleep.h"

#include "epd_driver.h"
#include "firasans.h"

static const char *TAG = "inky";

static uint8_t *framebuffer;

static int32_t draw_text(int32_t x, int32_t y, const char *text)
{
    int32_t cursor_x = x;
    int32_t cursor_y = y;
    FontProperties props = {
        .fg_color = 0,
        .bg_color = 0xF,
        .fallback_glyph = 0,
        .flags = DRAW_BACKGROUND,
    };
    write_mode(&FiraSans, text, &cursor_x, &cursor_y, framebuffer, BLACK_ON_WHITE, &props);
    return cursor_y;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Inky starting");

    epd_init();

    framebuffer = (uint8_t *)heap_caps_malloc(EPD_WIDTH / 2 * EPD_HEIGHT, MALLOC_CAP_SPIRAM);
    if (!framebuffer) {
        framebuffer = (uint8_t *)malloc(EPD_WIDTH / 2 * EPD_HEIGHT);
    }
    if (!framebuffer) {
        ESP_LOGE(TAG, "Failed to allocate framebuffer");
        return;
    }

    epd_poweron();
    epd_clear();

    epd_fill_rect(0, 0, EPD_WIDTH, EPD_HEIGHT, 0xFF, framebuffer);
    draw_text(50, 100, "Inky");

    epd_draw_grayscale_image(epd_full_screen(), framebuffer);
    epd_poweroff();

    ESP_LOGI(TAG, "Inky initialized; going to deep sleep");
    esp_sleep_enable_timer_wakeup(60 * 1000000ULL);
    esp_deep_sleep_start();
}

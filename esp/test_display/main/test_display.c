#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_heap_caps.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/gpio.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"

#include "epd_driver.h"
#include "firasans.h"
#include "lodepng.h"
#include "logo_png.h"

#define TAG "inky_test"

#define BATT_GPIO          14
#define SD_MISO_GPIO       16
#define SD_MOSI_GPIO       15
#define SD_SCLK_GPIO       11
#define SD_CS_GPIO         42
#define SD_HOST            SPI2_HOST

static const uint8_t *logo_png = _home_nanou_Code_Inky_esp_test_display_main_logo_200x200_png;
#define logo_png_len _home_nanou_Code_Inky_esp_test_display_main_logo_200x200_png_len

static uint8_t *framebuffer = NULL;

static void draw_logo(void)
{
    unsigned char *rgba = NULL;
    unsigned width = 0, height = 0;
    unsigned err = lodepng_decode32(&rgba, &width, &height, logo_png, logo_png_len);
    if (err || !rgba) {
        ESP_LOGE(TAG, "PNG decode failed: %s", lodepng_error_text(err));
        return;
    }

    int dst_x = 20;
    int dst_y = 20;
    int dst_w = (int)width;
    int dst_h = (int)height;

    for (int y = 0; y < dst_h; y++) {
        int sy = y;
        for (int x = 0; x < dst_w; x++) {
            int sx = x;
            uint8_t *px = rgba + (sy * width + sx) * 4;
            uint8_t r = px[0];
            uint8_t g = px[1];
            uint8_t b = px[2];
            uint8_t a = px[3];

            uint8_t gray = (uint8_t)(((uint16_t)r * 77 + (uint16_t)g * 150 + (uint16_t)b * 29) >> 8);

            // Pack 4-bit grayscale (0 = black, 15 = white)
            uint8_t epd = (uint8_t)((gray * 15 + 127) / 255);
            if (a < 128) {
                epd = 15; // transparent -> white
            }

            int dx = dst_x + x;
            int dy = dst_y + y;
            if (dx < 0 || dx >= EPD_WIDTH || dy < 0 || dy >= EPD_HEIGHT) {
                continue;
            }

            uint32_t idx = (dy * EPD_WIDTH + dx) / 2;
            if (dx & 1) {
                framebuffer[idx] = (framebuffer[idx] & 0xF0) | (epd & 0x0F);
            } else {
                framebuffer[idx] = (framebuffer[idx] & 0x0F) | (epd << 4);
            }
        }
    }

    free(rgba);
}

static void clear_fb(void)
{
    memset(framebuffer, 0xFF, EPD_WIDTH / 2 * EPD_HEIGHT);
}

static int32_t draw_text(int32_t x, int32_t y, const char *text)
{
    int32_t cursor_x = x;
    int32_t cursor_y = y;
    FontProperties props = {
        .fg_color = 0,
        .bg_color = 15,
        .fallback_glyph = 0,
        .flags = 0,
    };
    write_mode(&FiraSans, text, &cursor_x, &cursor_y, framebuffer, BLACK_ON_WHITE, &props);
    return cursor_y;
}

static void init_time(void)
{
    // Start with build date/time so the clock shows something plausible.
    struct tm tm = {0};
    const char *months[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    char mon_str[4] = {0};
    int day = 0, year = 0, hour = 0, min = 0, sec = 0;
    sscanf(__DATE__, "%3s %d %d", mon_str, &day, &year);
    sscanf(__TIME__, "%d:%d:%d", &hour, &min, &sec);
    for (int i = 0; i < 12; i++) {
        if (strncmp(mon_str, months[i], 3) == 0) {
            tm.tm_mon = i;
            break;
        }
    }
    tm.tm_mday = day;
    tm.tm_year = year - 1900;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_sec = sec;

    time_t t = mktime(&tm);
    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    settimeofday(&tv, NULL);
}

static void draw_time(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    draw_text(240, 100, buf);
}

static float read_battery_voltage(void)
{
    adc_oneshot_unit_handle_t adc_handle = NULL;
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = ADC_UNIT_2,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    if (adc_oneshot_new_unit(&init_cfg, &adc_handle) != ESP_OK) {
        return -1.0f;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    adc_oneshot_config_channel(adc_handle, ADC_CHANNEL_3, &chan_cfg);

    int raw = 0;
    int sum = 0;
    for (int i = 0; i < 16; i++) {
        if (adc_oneshot_read(adc_handle, ADC_CHANNEL_3, &raw) == ESP_OK) {
            sum += raw;
        } else {
            sum += 0;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    adc_oneshot_del_unit(adc_handle);

    float avg = sum / 16.0f;
    // ADC ref ~3.3V at 12-bit, 12dB attenuation gives roughly 0-3.1V range.
    // The LilyGo divider halves the battery voltage.
    float adc_voltage = (avg / 4095.0f) * 3.3f;
    float battery_voltage = adc_voltage * 2.0f;
    return battery_voltage;
}

RTC_DATA_ATTR float prev_v = -1.0f;

static void draw_battery(void)
{
    float v = read_battery_voltage();
    char buf[64];
    if (v < 0) {
        snprintf(buf, sizeof(buf), "Battery: error");
    } else {
        int pct = (int)((v - 3.0f) / (4.2f - 3.0f) * 100.0f);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;

        const char *trend = "-";
        if (prev_v > 0) {
            if (v > prev_v + 0.02f) {
                trend = "^ charging";
            } else if (v < prev_v - 0.02f) {
                trend = "v discharging";
            }
        }
        prev_v = v;

        snprintf(buf, sizeof(buf), "Battery: %.2fV (%d%%) %s", v, pct, trend);
    }
    draw_text(240, 140, buf);
}

static void draw_sd_card(void)
{
    char buf[64] = "SD: not detected";

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_HOST;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_MOSI_GPIO,
        .miso_io_num = SD_MISO_GPIO,
        .sclk_io_num = SD_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };

    esp_err_t ret = spi_bus_initialize((spi_host_device_t)host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
    } else {
        sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
        slot_config.gpio_cs = SD_CS_GPIO;
        slot_config.host_id = (spi_host_device_t)host.slot;

        sdmmc_card_t *card = NULL;
        esp_vfs_fat_sdmmc_mount_config_t mount_config = {
            .format_if_mount_failed = false,
            .max_files = 1,
            .allocation_unit_size = 16 * 1024,
        };

        ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_config, &mount_config, &card);
        if (ret == ESP_OK && card) {
            uint64_t size_bytes = (uint64_t)card->csd.capacity * card->csd.sector_size;
            double size_gb = size_bytes / (1024.0 * 1024.0 * 1024.0);
            snprintf(buf, sizeof(buf), "SD: %.2f GB", size_gb);
            esp_vfs_fat_sdcard_unmount("/sdcard", card);
        } else {
            ESP_LOGW(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        }
        spi_bus_free((spi_host_device_t)host.slot);
    }

    draw_text(240, 180, buf);
}

static void draw_screen(void)
{
    clear_fb();
    draw_logo();

    draw_text(240, 60, "Inky test display");
    draw_time();
    draw_battery();
    draw_sd_card();

    Rect_t area = {
        .x = 0,
        .y = 0,
        .width = EPD_WIDTH,
        .height = EPD_HEIGHT,
    };
    epd_draw_grayscale_image(area, framebuffer);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Inky test display starting");

    // Only set the clock on a cold boot; RTC time keeps running across deep sleep.
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER) {
        init_time();
    }

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

    draw_screen();

    epd_poweroff();

    // Refresh every 60 seconds, deep sleep in between.
    ESP_LOGI(TAG, "Going to deep sleep for 60 s");
    esp_sleep_enable_timer_wakeup(60 * 1000000ULL);
    esp_deep_sleep_start();
}

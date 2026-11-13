#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"

#include "epd_driver.h"
#include "firasans.h"

#include "archive/archive.h"
#include "epub/epub.h"
#include "html/html.h"
#include "layout/layout.h"

static const char *TAG = "inky";

#define SD_MISO_GPIO       16
#define SD_MOSI_GPIO       15
#define SD_SCLK_GPIO       11
#define SD_CS_GPIO         42
#define SD_HOST            SPI2_HOST

#define SD_MOUNT_POINT     "/sdcard"

static uint8_t *framebuffer;
static int32_t cursor_y;
static sdmmc_card_t *sdcard = NULL;

static int32_t draw_text(int32_t x, int32_t y, const char *text)
{
    int32_t cursor_x = x;
    int32_t cursor_y_local = y;
    FontProperties props = {
        .fg_color = 0,
        .bg_color = 0xF,
        .fallback_glyph = 0,
        .flags = DRAW_BACKGROUND,
    };
    write_mode(&FiraSans, text, &cursor_x, &cursor_y_local, framebuffer, BLACK_ON_WHITE, &props);
    return cursor_y_local;
}

static void draw_line(const char *text)
{
    cursor_y = draw_text(20, cursor_y, text);
    cursor_y += 10;
}

static void render_layout_line(int line_number, const char *line_text,
                               int is_status, void *user_data)
{
    (void)line_number;
    (void)is_status;
    (void)user_data;
    draw_line(line_text);
}

static int mount_sdcard(void)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_HOST;
    host.max_freq_khz = 4000;

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
        return -1;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_CS_GPIO;
    slot_config.host_id = (spi_host_device_t)host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 2,
        .allocation_unit_size = 16 * 1024,
    };

    ret = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_config, &mount_config, &sdcard);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        spi_bus_free((spi_host_device_t)host.slot);
        return -1;
    }

    ESP_LOGI(TAG, "SD card mounted");
    return 0;
}

static void unmount_sdcard(void)
{
    if (sdcard) {
        esp_vfs_fat_sdcard_unmount("/sdcard", sdcard);
        sdcard = NULL;
    }
    spi_bus_free((spi_host_device_t)SD_HOST);
}

static void render_error(const char *msg)
{
    epd_fill_rect(0, 0, EPD_WIDTH, EPD_HEIGHT, 0xFF, framebuffer);
    cursor_y = 50;
    draw_line("Inky");
    draw_line(msg);
    epd_draw_grayscale_image(epd_full_screen(), framebuffer);
}

static int find_epub_file(char *path_out, size_t path_size)
{
    DIR *dir = opendir(SD_MOUNT_POINT);
    if (!dir) return -1;

    struct dirent *entry;
    int found = -1;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        size_t len = strlen(name);
        if (len < 5) continue;
        const char *ext = name + len - 4;
        if (strcasecmp(ext, ".epub") == 0 ||
            strcasecmp(ext, ".epu") == 0) {
            snprintf(path_out, path_size, "%s/%s", SD_MOUNT_POINT, name);
            found = 0;
            break;
        }
    }
    closedir(dir);
    return found;
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

    if (mount_sdcard() != 0) {
        render_error("SD mount failed");
        epd_poweroff();
        goto halt;
    }

    char epub_path[128];
    if (find_epub_file(epub_path, sizeof(epub_path)) != 0) {
        ESP_LOGE(TAG, "No .epub file found on SD card");
        render_error("No .epub file found");
        unmount_sdcard();
        epd_poweroff();
        goto halt;
    }

    epub_t *epub = epub_open(epub_path);
    if (!epub) {
        render_error("Cannot open EPUB");
        unmount_sdcard();
        epd_poweroff();
        goto halt;
    }

    char header[128];
    snprintf(header, sizeof(header), "Chapters: %d", epub->chapter_count);

    if (epub->chapter_count > 0) {
        size_t len;
        char *html = epub_read_file(epub, epub->chapters[0].path, &len);
        if (html) {
            size_t text_size = len * 2 + 1;
            char *text = malloc(text_size);
            if (text) {
                html_to_text(html, text, text_size);
                layout_t *layout = layout_new();
                if (layout && layout_build(layout, text, 40)) {
                    epd_fill_rect(0, 0, EPD_WIDTH, EPD_HEIGHT, 0xFF, framebuffer);
                    cursor_y = 30;
                    draw_line(header);
                    snprintf(header, sizeof(header), "Path: %s", epub->chapters[0].path);
                    draw_line(header);

                    int rows = 12;
                    layout_render_page(layout, 0, rows, render_layout_line, NULL);

                    epd_draw_grayscale_image(epd_full_screen(), framebuffer);
                    layout_free(layout);
                } else {
                    render_error("Layout build failed");
                }
                free(text);
            } else {
                render_error("Text buffer alloc failed");
            }
            free(html);
        } else {
            render_error("Cannot read chapter 0");
        }
    } else {
        render_error("No chapters found");
    }

    epub_close(epub);
    unmount_sdcard();
    epd_poweroff();

halt:
    ESP_LOGI(TAG, "Inky debug halt");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

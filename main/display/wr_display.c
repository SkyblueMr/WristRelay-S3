// SPDX-License-Identifier: MIT

#include "wr_display.h"
#include "sdkconfig.h"

#if CONFIG_WR_DISPLAY_WAVESHARE_206

#include <stdint.h>
#include <string.h>
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "waveshare_206_init.h"

enum { WIDTH = 410, HEIGHT = 502, BAND = 8, STATUS_TOP = 192, STATUS_BOTTOM = 288 };

typedef struct {
    char letter;
    uint8_t row[7];
} glyph_t;

static const glyph_t glyphs[] = {
    {'A', {14, 17, 17, 31, 17, 17, 17}},
    {'C', {15, 16, 16, 16, 16, 16, 15}},
    {'D', {30, 17, 17, 17, 17, 17, 30}},
    {'E', {31, 16, 16, 30, 16, 16, 31}},
    {'G', {15, 16, 16, 19, 17, 17, 15}},
    {'I', {31, 4, 4, 4, 4, 4, 31}},
    {'L', {16, 16, 16, 16, 16, 16, 31}},
    {'N', {17, 25, 25, 21, 19, 19, 17}},
    {'O', {14, 17, 17, 17, 17, 17, 14}},
    {'R', {30, 17, 17, 30, 20, 18, 17}},
    {'S', {15, 16, 16, 14, 1, 1, 30}},
    {'T', {31, 4, 4, 4, 4, 4, 4}},
    {'U', {17, 17, 17, 17, 17, 17, 14}},
    {'V', {17, 17, 17, 17, 17, 10, 4}},
    {'W', {17, 17, 17, 21, 21, 27, 17}},
    {'Y', {17, 17, 10, 4, 4, 4, 4}},
};

static const struct {
    const char *name;
    uint16_t color;
} status_style[] = {
    [WR_DISPLAY_IDLE] = {"IDLE", 0x9E79},
    [WR_DISPLAY_CONNECTING] = {"CONNECTING", 0xFE66},
    [WR_DISPLAY_AUDIO] = {"AUDIO ACTIVE", 0x4F74},
    [WR_DISPLAY_ERROR] = {"ERROR", 0xFB0B},
};

static esp_lcd_panel_handle_t panel;
static esp_lcd_panel_io_handle_t panel_io;
static uint16_t *pixels;
static bool awake;
static bool dirty;
static wr_display_status_t next_status;

static uint16_t wire_color(uint16_t rgb)
{
    return (uint16_t)((rgb >> 8) | (rgb << 8));
}

static void label_row(uint16_t *row, const char *text, int y, int scale, uint16_t rgb)
{
    if (y < 0 || y >= 7 * scale) {
        return;
    }
    int x = (WIDTH - ((int)strlen(text) * 6 - 1) * scale) / 2;
    for (; *text; ++text, x += 6 * scale) {
        for (size_t i = 0; i < sizeof(glyphs) / sizeof(glyphs[0]); ++i) {
            if (glyphs[i].letter != *text) {
                continue;
            }
            const uint8_t mask = glyphs[i].row[y / scale];
            for (int col = 0; col < 5; ++col) {
                if (mask & (1U << (4 - col))) {
                    for (int n = 0; n < scale; ++n) {
                        row[x + col * scale + n] = wire_color(rgb);
                    }
                }
            }
            break;
        }
    }
}

static esp_err_t paint(int first, int last)
{
    for (int y = first; y < last; y += BAND) {
        const int count = last - y < BAND ? last - y : BAND;
        memset(pixels, 0, WIDTH * count * sizeof(*pixels));
        for (int n = 0; n < count; ++n) {
            const int line = y + n;
            uint16_t *row = pixels + n * WIDTH;
            label_row(row, "WRISTRELAY", line - 132, 4, 0xFFFF);
            label_row(row, status_style[next_status].name, line - 220, 5,
                      status_style[next_status].color);
            if (line >= 268 && line < 272) {
                for (int x = 163; x < 247; ++x) {
                    row[x] = wire_color(status_style[next_status].color);
                }
            }
        }
        esp_err_t err = esp_lcd_panel_draw_bitmap(panel, 0, y, WIDTH, y + count, pixels);
        if (err != ESP_OK) {
            return err;
        }
        // A no-command parameter transaction drains the SPI color queue before reuse.
        err = esp_lcd_panel_io_tx_param(panel_io, -1, NULL, 0);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t wr_display_init(i2c_master_bus_handle_t bus)
{
    (void)bus;
    if (panel) {
        return ESP_ERR_INVALID_STATE;
    }
    const size_t bytes = WIDTH * BAND * sizeof(*pixels);
    pixels = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!pixels) {
        return ESP_ERR_NO_MEM;
    }
    const spi_bus_config_t spi = CO5300_PANEL_BUS_QSPI_CONFIG(11, 4, 5, 6, 7, bytes);
    const esp_lcd_panel_io_spi_config_t io = CO5300_PANEL_IO_QSPI_CONFIG(12, NULL, NULL);
    co5300_vendor_config_t vendor = {
        .init_cmds = wr_waveshare_206_init,
        .init_cmds_size = sizeof(wr_waveshare_206_init) / sizeof(wr_waveshare_206_init[0]),
        .flags.use_qspi_interface = 1,
    };
    const esp_lcd_panel_dev_config_t config = {
        .reset_gpio_num = 8,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &spi, SPI_DMA_CH_AUTO);
    const bool bus_started = err == ESP_OK;
    if (err == ESP_OK) err = esp_lcd_new_panel_io_spi(SPI2_HOST, &io, &panel_io);
    if (err == ESP_OK) err = esp_lcd_new_panel_co5300(panel_io, &config, &panel);
    if (err == ESP_OK) err = esp_lcd_panel_reset(panel);
    if (err == ESP_OK) err = esp_lcd_panel_init(panel);
    if (err == ESP_OK) err = esp_lcd_panel_set_gap(panel, 22, 0);
    if (err == ESP_OK) err = esp_lcd_panel_disp_on_off(panel, false);
    if (err == ESP_OK) err = paint(0, HEIGHT);
    if (err == ESP_OK) err = esp_lcd_panel_disp_on_off(panel, true);
    if (err == ESP_OK) err = esp_lcd_panel_co5300_set_brightness(panel, 56);
    if (err == ESP_OK) {
        awake = true;
        return ESP_OK;
    }
    if (panel) esp_lcd_panel_del(panel);
    if (panel_io) esp_lcd_panel_io_del(panel_io);
    if (bus_started) spi_bus_free(SPI2_HOST);
    heap_caps_free(pixels);
    panel = NULL;
    panel_io = NULL;
    pixels = NULL;
    return err;
}

esp_err_t wr_display_status(wr_display_status_t status)
{
    if ((unsigned)status >= sizeof(status_style) / sizeof(status_style[0])) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!panel) {
        return ESP_ERR_INVALID_STATE;
    }
    dirty |= status != next_status;
    next_status = status;
    if (!awake || !dirty) {
        return ESP_OK;
    }
    const esp_err_t err = paint(STATUS_TOP, STATUS_BOTTOM);
    dirty = err != ESP_OK;
    return err;
}

esp_err_t wr_display_set_awake(bool on)
{
    if (!panel) {
        return ESP_ERR_INVALID_STATE;
    }
    if (on == awake) {
        return ESP_OK;
    }
    if (on && dirty) {
        const esp_err_t err = paint(STATUS_TOP, STATUS_BOTTOM);
        if (err != ESP_OK) {
            return err;
        }
        dirty = false;
    }
    const esp_err_t err = esp_lcd_panel_disp_on_off(panel, on);
    if (err == ESP_OK) {
        awake = on;
    }
    return err;
}

#else

esp_err_t wr_display_init(i2c_master_bus_handle_t bus)
{
    (void)bus;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t wr_display_status(wr_display_status_t status)
{
    (void)status;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t wr_display_set_awake(bool awake)
{
    (void)awake;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif

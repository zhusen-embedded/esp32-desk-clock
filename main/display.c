#include "display.h"
#include "lvgl.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/spi_master.h"
#include "esp_log.h"

static const char *TAG = "display";

#define LCD_H_RES       240
#define LCD_V_RES       320
#define LCD_BUF_LINES   40

static esp_lcd_panel_handle_t s_panel;
static lv_disp_draw_buf_t s_draw_buf;
static lv_color_t s_buf1[LCD_H_RES * LCD_BUF_LINES];
static lv_color_t s_buf2[LCD_H_RES * LCD_BUF_LINES];

/* LVGL 刷屏回调：把渲染好的区域推给屏 */
static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color)
{
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, color);
    lv_disp_flush_ready(drv);
}

void display_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = 11, .miso_io_num = -1,
        .sclk_io_num = 12,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_BUF_LINES * sizeof(uint16_t),
    };
    spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = 9, .cs_gpio_num = 10,
        .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .spi_mode = 0, .trans_queue_depth = 10,
    };
    esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io);

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = 8,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel);

    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    /* 240×320 面板常见 0/0 偏移，设错画面会错位 */
    esp_lcd_panel_set_gap(s_panel, 0, 0);
    esp_lcd_panel_invert_color(s_panel, true);
    esp_lcd_panel_disp_on_off(s_panel, true);

    lv_init();
    lv_disp_draw_buf_init(&s_draw_buf, s_buf1, s_buf2, LCD_H_RES * LCD_BUF_LINES);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_H_RES;
    disp_drv.ver_res = LCD_V_RES;
    disp_drv.flush_cb = flush_cb;
    disp_drv.draw_buf = &s_draw_buf;
    lv_disp_drv_register(&disp_drv);

    ESP_LOGI(TAG, "st7789 ready, dual buffer %d lines", LCD_BUF_LINES);
}

#include "display.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <assert.h>

static const char *TAG = "display";

#define LCD_BUF_LINES       40
#define LCD_PIN_MOSI        11
#define LCD_PIN_SCLK        12
#define LCD_PIN_CS          10
#define LCD_PIN_DC          9
#define LCD_PIN_RST         8
#define LCD_PIN_BL          5

#define LCD_SPI_HOST        SPI2_HOST
#define LCD_PCLK_HZ         (40 * 1000 * 1000)

#define BL_LEDC_TIMER       LEDC_TIMER_0
#define BL_LEDC_CHANNEL     LEDC_CHANNEL_0
#define BL_LEDC_MODE        LEDC_LOW_SPEED_MODE
#define BL_LEDC_RES         LEDC_TIMER_10_BIT
#define BL_LEDC_FREQ_HZ     5000

static esp_lcd_panel_handle_t s_panel;
static lv_display_t *s_disp;

/* LVGL 9 用 lv_display_set_buffers 接管缓冲，缓冲区由我们分配，
 * 大小 = 水平分辨率 x 行数 x 2 字节（RGB565）。 */
static void *s_buf1;
static void *s_buf2;

/* 刷屏完成信号：LVGL 把数据交给 DMA 后必须先返回，
 * 等 transfer_done 回调再调 lv_display_flush_ready，否则双缓冲会写同一块。
 * 原实现直接同步 ready，等于退化成单缓冲。 */
static SemaphoreHandle_t s_flush_done;

static bool on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                esp_lcd_panel_io_event_data_t *edata,
                                void *user_ctx)
{
    BaseType_t need_yield = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &need_yield);
    return need_yield == pdTRUE;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1, px_map);
    /* 等这一块真的传完，再告诉 LVGL 可以复用缓冲区 */
    xSemaphoreTake(s_flush_done, portMAX_DELAY);
    lv_display_flush_ready(disp);
}

static void backlight_pwm_init(void)
{
    ledc_timer_config_t t = {
        .speed_mode = BL_LEDC_MODE,
        .timer_num = BL_LEDC_TIMER,
        .duty_resolution = BL_LEDC_RES,
        .freq_hz = BL_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t));

    ledc_channel_config_t c = {
        .gpio_num = LCD_PIN_BL,
        .speed_mode = BL_LEDC_MODE,
        .channel = BL_LEDC_CHANNEL,
        .timer_sel = BL_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&c));
}

void display_set_backlight(uint8_t percent)
{
    if (percent > 100) percent = 100;
    /* 10 bit 分辨率，满量程 1023 */
    uint32_t duty = (1023u * percent) / 100u;
    ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty);
    ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
}

void display_init(void)
{
    s_flush_done = xSemaphoreCreateBinary();

    backlight_pwm_init();

    spi_bus_config_t bus = {
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = LCD_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_BUF_LINES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = LCD_PIN_DC,
        .cs_gpio_num = LCD_PIN_CS,
        .pclk_hz = LCD_PCLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = on_color_trans_done,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &io));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    /* 240×320 面板常见 0/0 偏移，设错画面会错位 */
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, 0, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    lv_init();

    /* LVGL 9：先建 display 对象，再挂缓冲和回调 */
    s_disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);

    size_t buf_bytes = LCD_H_RES * LCD_BUF_LINES * sizeof(uint16_t);
    s_buf1 = spi_bus_dma_memory_alloc(LCD_SPI_HOST, buf_bytes, 0);
    s_buf2 = spi_bus_dma_memory_alloc(LCD_SPI_HOST, buf_bytes, 0);
    assert(s_buf1 && s_buf2);

    /* FULL 模式：LVGL 自己会挑空闲缓冲，配合 flush_ready 就是真双缓冲 */
    lv_display_set_buffers(s_disp, s_buf1, s_buf2, buf_bytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, flush_cb);

    display_set_backlight(60);
    ESP_LOGI(TAG, "st7789 ready, dual buffer %d lines (%u B each)",
             LCD_BUF_LINES, (unsigned)buf_bytes);
}

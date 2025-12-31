#include "ui.h"

#include "lvgl.h"
#include "display.h"
#include "esp_log.h"

static const char *TAG = "ui";

static lv_obj_t *s_pages[UI_PAGE_NUM];
static ui_page_t s_cur;

/* 表盘 */
static lv_obj_t *s_lbl_time;
static lv_obj_t *s_lbl_date;
static lv_obj_t *s_lbl_net;

/* 天气 */
static lv_obj_t *s_lbl_weather_temp;
static lv_obj_t *s_lbl_weather_text;

/* 设置 */
static lv_obj_t *s_slider_bl;
static lv_obj_t *s_lbl_bl;

static const char *WEEKDAY_CN[] = {"日", "一", "二", "三", "四", "五", "六"};

static void show_page(ui_page_t p)
{
    for (int i = 0; i < UI_PAGE_NUM; i++) {
        if (s_pages[i] == NULL) continue;
        if ((ui_page_t)i == p) {
            lv_obj_clear_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    s_cur = p;
}

static lv_obj_t *make_page(void)
{
    lv_obj_t *o = lv_obj_create(lv_scr_act());
    lv_obj_set_size(o, LCD_H_RES, LCD_V_RES);
    lv_obj_center(o);
    lv_obj_set_style_bg_color(o, lv_color_black(), 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *make_label(lv_obj_t *parent, int y, const lv_font_t *font,
                            lv_color_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    return l;
}

/* 手势回调：挂在根屏上，全局生效 */
static void gesture_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_get_act();
    if (indev == NULL) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);

    if (dir == LV_DIR_LEFT) {
        ui_next_page();
    } else if (dir == LV_DIR_RIGHT) {
        ui_prev_page();
    }
}

static void bl_slider_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int32_t v = lv_slider_get_value(slider);
    display_set_backlight((uint8_t)v);

    char buf[16];
    lv_snprintf(buf, sizeof(buf), "%d%%", (int)v);
    lv_label_set_text(s_lbl_bl, buf);
}

void ui_init(void)
{
    /* ---- 页面 1：表盘 ---- */
    s_pages[UI_PAGE_CLOCK] = make_page();
    s_lbl_time = make_label(s_pages[UI_PAGE_CLOCK], 90,
                            &lv_font_montserrat_48, lv_color_white(), "00:00");
    s_lbl_date = make_label(s_pages[UI_PAGE_CLOCK], 160,
                            &lv_font_montserrat_20, lv_color_hex(0xAAAAAA), "----/--/--");
    s_lbl_net  = make_label(s_pages[UI_PAGE_CLOCK], 200,
                            &lv_font_montserrat_14, lv_color_hex(0x666666), "未同步");

    /* ---- 页面 2：天气 ---- */
    s_pages[UI_PAGE_WEATHER] = make_page();
    make_label(s_pages[UI_PAGE_WEATHER], 60, &lv_font_montserrat_20,
               lv_color_hex(0x888888), "WEATHER");
    s_lbl_weather_temp = make_label(s_pages[UI_PAGE_WEATHER], 110,
                                    &lv_font_montserrat_48, lv_color_white(), "-- C");
    s_lbl_weather_text = make_label(s_pages[UI_PAGE_WEATHER], 180,
                                    &lv_font_montserrat_20, lv_color_hex(0xAAAAAA), "--");

    /* ---- 页面 3：设置 ---- */
    s_pages[UI_PAGE_SETTINGS] = make_page();
    make_label(s_pages[UI_PAGE_SETTINGS], 40, &lv_font_montserrat_20,
               lv_color_hex(0x888888), "BACKLIGHT");

    s_slider_bl = lv_slider_create(s_pages[UI_PAGE_SETTINGS]);
    lv_slider_set_range(s_slider_bl, 5, 100);
    lv_slider_set_value(s_slider_bl, 60, LV_ANIM_OFF);
    lv_obj_set_width(s_slider_bl, 160);
    lv_obj_align(s_slider_bl, LV_ALIGN_TOP_MID, 0, 90);
    lv_obj_add_event_cb(s_slider_bl, bl_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_lbl_bl = make_label(s_pages[UI_PAGE_SETTINGS], 130,
                          &lv_font_montserrat_20, lv_color_white(), "60%");

    /* 手势注册在活动屏上 */
    lv_obj_add_event_cb(lv_scr_act(), gesture_cb, LV_EVENT_GESTURE, NULL);

    show_page(UI_PAGE_CLOCK);
    ESP_LOGI(TAG, "ui ready, %d pages", UI_PAGE_NUM);
}

void ui_next_page(void)
{
    show_page((ui_page_t)((s_cur + 1) % UI_PAGE_NUM));
}

void ui_prev_page(void)
{
    show_page((ui_page_t)((s_cur + UI_PAGE_NUM - 1) % UI_PAGE_NUM));
}

ui_page_t ui_current_page(void) { return s_cur; }

void ui_set_time(int hour, int min, int sec)
{
    char buf[16];
    lv_snprintf(buf, sizeof(buf), "%02d:%02d", hour, min);
    lv_label_set_text(s_lbl_time, buf);
    (void)sec;
}

void ui_set_date(int month, int day, int weekday)
{
    char buf[24];
    if (weekday < 0 || weekday > 6) weekday = 0;
    lv_snprintf(buf, sizeof(buf), "%02d/%02d  周%s", month, day, WEEKDAY_CN[weekday]);
    lv_label_set_text(s_lbl_date, buf);
}

void ui_set_weather(int temp_c, const char *text, bool valid)
{
    char buf[24];
    if (valid) {
        lv_snprintf(buf, sizeof(buf), "%d C", temp_c);
    } else {
        lv_snprintf(buf, sizeof(buf), "-- C");
    }
    lv_label_set_text(s_lbl_weather_temp, buf);
    lv_label_set_text(s_lbl_weather_text, (valid && text) ? text : "--");
}

void ui_set_net_status(bool wifi_ok, bool time_synced)
{
    if (!wifi_ok) {
        lv_label_set_text(s_lbl_net, "Wi-Fi 未连接");
        lv_obj_set_style_text_color(s_lbl_net, lv_color_hex(0xCC4444), 0);
    } else if (!time_synced) {
        lv_label_set_text(s_lbl_net, "已联网 / 未校时");
        lv_obj_set_style_text_color(s_lbl_net, lv_color_hex(0xCCAA44), 0);
    } else {
        lv_label_set_text(s_lbl_net, "已同步");
        lv_obj_set_style_text_color(s_lbl_net, lv_color_hex(0x44AA66), 0);
    }
}

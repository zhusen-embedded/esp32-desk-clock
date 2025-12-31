#ifndef UI_H
#define UI_H

#include <stdbool.h>

/* 三个页面：表盘 / 天气 / 设置。
 * 用独立的 lv_obj 作为页面而不是 lv_scr_load，切换时只改可见性，
 * 避免每次切页都重建控件（原实现用 lv_scr_load_anim 把当前屏当目标，等于没切）。 */

typedef enum {
    UI_PAGE_CLOCK = 0,
    UI_PAGE_WEATHER,
    UI_PAGE_SETTINGS,
    UI_PAGE_NUM,
} ui_page_t;

void ui_init(void);
void ui_next_page(void);
void ui_prev_page(void);
ui_page_t ui_current_page(void);

/* 由业务层调用刷新数据，UI 只负责显示 */
void ui_set_time(int hour, int min, int sec);
void ui_set_date(int month, int day, int weekday);
void ui_set_weather(int temp_c, const char *text, bool valid);
void ui_set_net_status(bool wifi_ok, bool time_synced);

#endif

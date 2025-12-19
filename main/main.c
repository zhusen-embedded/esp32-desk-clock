#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lvgl.h"
#include "display.h"
#include "net_time.h"
#include "ble_prov.h"

static const char *TAG = "main";

/* LVGL 需要周期性调 lv_timer_handler，5 ms 一次保证 30 FPS */
static void lvgl_task(void *arg)
{
    while (1) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/* 手势切换页面：左右滑动在表盘/天气/设置之间切 */
static void gesture_cb(lv_event_t *e)
{
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    static uint8_t page;

    if (dir == LV_DIR_LEFT) {
        page = (page + 1) % 3;
    } else if (dir == LV_DIR_RIGHT) {
        page = (page + 2) % 3;
    }
    /* 切换页面动画 */
    lv_scr_load_anim(lv_scr_act(), LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}

void app_main(void)
{
    display_init();
    net_time_start();
    net_weather_fetch();

    xTaskCreate(lvgl_task, "lvgl", 8192, NULL, 4, NULL);

    ESP_LOGI(TAG, "desk clock started");
}

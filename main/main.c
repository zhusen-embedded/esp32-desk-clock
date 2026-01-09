#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include <time.h>

#include "display.h"
#include "net_time.h"
#include "ble_prov.h"
#include "ui.h"

static const char *TAG = "main";

/* LVGL 不是线程安全的：所有 lv_* 调用必须持锁。
 * 原实现只有 lvgl_task 里调 lv_timer_handler，业务任务直接改控件，
 * 在双核上偶发花屏/重启就是这个原因。 */
static SemaphoreHandle_t s_lvgl_lock;

bool ui_lock(uint32_t timeout_ms)
{
    return xSemaphoreTake(s_lvgl_lock, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void ui_unlock(void)
{
    xSemaphoreGive(s_lvgl_lock);
}

/* 5 ms 一次 tick，保证 30 FPS；LVGL9 用 lv_timer_handler */
static void lvgl_task(void *arg)
{
    uint32_t wait = 5;
    while (1) {
        if (ui_lock(10)) {
            wait = lv_timer_handler();
            ui_unlock();
        }
        if (wait < 5) wait = 5;
        if (wait > 20) wait = 20;       /* 别睡太久，否则帧率掉 */
        vTaskDelay(pdMS_TO_TICKS(wait));
    }
}

/* 每秒刷一次时间显示 */
static void clock_task(void *arg)
{
    while (1) {
        time_t now = 0;
        struct tm t = {0};
        time(&now);
        localtime_r(&now, &t);

        if (ui_lock(100)) {
            ui_set_time(t.tm_hour, t.tm_min, t.tm_sec);
            ui_set_date(t.tm_mon + 1, t.tm_mday, t.tm_wday);
            ui_set_net_status(true, net_time_synced());
            ui_unlock();
        }

        /* 夜间熄屏：22:00~07:00 背光降到 10% */
        bool night = (t.tm_hour >= 22 || t.tm_hour < 7);
        display_set_backlight(night ? 10 : 60);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* 天气数据变了就刷界面，不轮询 */
static void weather_ui_task(void *arg)
{
    int last_temp = 0x7FFFFFFF;
    while (1) {
        const weather_t *w = net_weather_get();
        if (w->valid && w->temp_c != last_temp) {
            last_temp = w->temp_c;
            if (ui_lock(100)) {
                ui_set_weather(w->temp_c, w->text, w->valid);
                ui_unlock();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

static void on_prov_done(bool ok, const char *ssid)
{
    ESP_LOGI(TAG, "provision %s, ssid=%s", ok ? "ok" : "failed", ssid ? ssid : "-");
}

void app_main(void)
{
    s_lvgl_lock = xSemaphoreCreateMutex();

    display_init();
    ui_init();

    xTaskCreate(lvgl_task, "lvgl", 8192, NULL, 4, NULL);
    xTaskCreate(clock_task, "clock", 4096, NULL, 3, NULL);
    xTaskCreate(weather_ui_task, "wui", 3072, NULL, 2, NULL);

    net_time_start();
    net_weather_start(30);

    /* 没存过 Wi-Fi 才开 BLE 配网 */
    char ssid[33] = {0}, pass[65] = {0};
    if (!ble_prov_load(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGI(TAG, "no saved wifi, start BLE provisioning");
        ble_prov_start(on_prov_done);
    } else {
        ESP_LOGI(TAG, "saved wifi: %s", ssid);
    }

    ESP_LOGI(TAG, "desk clock started");
}

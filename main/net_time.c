#include "net_time.h"

#include "esp_sntp.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "nettime";

#define WEATHER_URL     "http://api.example.com/weather?city=guangzhou"
#define HTTP_BUF_SIZE   512

static weather_t s_weather;
static volatile bool s_synced;
static char s_http_buf[HTTP_BUF_SIZE];
static int  s_http_len;

static bool sntp_sync_cb_called;

static void on_sntp_sync(struct timeval *tv)
{
    sntp_sync_cb_called = true;
    s_synced = true;
    ESP_LOGI(TAG, "sntp synced");
}

void net_time_start(void)
{
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_set_time_sync_notification_cb(on_sntp_sync);
    esp_sntp_init();

    /* 等首次同步，最多 10 s，超时就先用本地 RTC 走。
     * 用回调标志而不是轮询 sntp_get_sync_status()，后者在 POLL 模式下
     * 完成瞬间可能被重置，原实现会出现「明明同步了却报超时」。 */
    int waited = 0;
    while (!sntp_sync_cb_called && waited < 10000) {
        vTaskDelay(pdMS_TO_TICKS(200));
        waited += 200;
    }
    if (!sntp_sync_cb_called) {
        ESP_LOGW(TAG, "sntp timeout after %d ms, fallback to local rtc", waited);
    }
}

bool net_time_synced(void) { return s_synced; }

/* ---- 天气 ---- */

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ON_CONNECTED:
        s_http_len = 0;
        break;
    case HTTP_EVENT_ON_DATA:
        /* 原实现每收到一段数据就 cJSON_Parse 一次，包被 TCP 拆开时必然解析失败。
         * 正确做法是先攒到缓冲区，等 HTTP_EVENT_ON_FINISH 再整体解析。 */
        if (s_http_len + evt->data_len < (int)sizeof(s_http_buf) - 1) {
            memcpy(s_http_buf + s_http_len, evt->data, evt->data_len);
            s_http_len += evt->data_len;
            s_http_buf[s_http_len] = '\0';
        }
        break;
    case HTTP_EVENT_ON_FINISH: {
        if (s_http_len == 0) break;
        cJSON *root = cJSON_Parse(s_http_buf);
        if (root == NULL) {
            ESP_LOGW(TAG, "weather json parse failed");
            break;
        }
        cJSON *temp = cJSON_GetObjectItem(root, "temp");
        cJSON *text = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsNumber(temp)) s_weather.temp_c = temp->valueint;
        if (cJSON_IsString(text) && text->valuestring) {
            strncpy(s_weather.text, text->valuestring, sizeof(s_weather.text) - 1);
        }
        s_weather.valid = cJSON_IsNumber(temp);
        cJSON_Delete(root);
        if (s_weather.valid) {
            ESP_LOGI(TAG, "weather: %d C %s", s_weather.temp_c, s_weather.text);
        }
        break;
    }
    case HTTP_EVENT_DISCONNECTED:
        break;
    default:
        break;
    }
    return ESP_OK;
}

static void weather_task(void *arg)
{
    int interval_min = (int)(intptr_t)arg;
    if (interval_min <= 0) interval_min = 30;

    while (1) {
        /* 没同步到时间就不拉天气（证书校验要时间） */
        if (s_synced || net_time_synced()) {
            esp_http_client_config_t cfg = {
                .url = WEATHER_URL,
                .event_handler = http_event,
                .timeout_ms = 5000,
                .buffer_size = 256,
            };
            esp_http_client_handle_t c = esp_http_client_init(&cfg);
            if (c) {
                esp_err_t err = esp_http_client_perform(c);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "weather fetch failed: %s", esp_err_to_name(err));
                }
                esp_http_client_cleanup(c);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(interval_min * 60 * 1000));
    }
}

void net_weather_start(int interval_min)
{
    xTaskCreate(weather_task, "weather", 4096, (void *)(intptr_t)interval_min, 3, NULL);
}

const weather_t *net_weather_get(void) { return &s_weather; }

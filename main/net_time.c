#include "net_time.h"
#include "esp_sntp.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include <time.h>
#include <string.h>

static const char *TAG = "nettime";
static char s_weather[32] = "--";
static int  s_temp;

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        /* 天气 API 返回 JSON，用 cJSON 解析出温度和天气 */
        cJSON *root = cJSON_ParseWithLength(evt->data, evt->data_len);
        if (root) {
            cJSON *temp = cJSON_GetObjectItem(root, "temp");
            if (cJSON_IsNumber(temp)) s_temp = temp->valueint;
            cJSON_Delete(root);
        }
    }
    return ESP_OK;
}

void net_time_start(void)
{
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_init();

    /* 等首次同步，最多 10 s，超时就先用本地 RTC 走 */
    int retry = 0;
    while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && retry++ < 20) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (retry >= 20) {
        ESP_LOGW(TAG, "sntp timeout, fallback to local rtc");
    }
}

void net_weather_fetch(void)
{
    esp_http_client_config_t cfg = {
        .url = "http://api.example.com/weather?city=guangzhou",
        .event_handler = http_event,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (esp_http_client_perform(c) != ESP_OK) {
        ESP_LOGW(TAG, "weather fetch failed");
    }
    esp_http_client_cleanup(c);
}

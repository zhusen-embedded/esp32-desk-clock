#include "ble_prov.h"
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "nvs_flash.h"
#include "nvs.h"

/* 自定义 GATT 服务：
 *   手机连上后往 0x2A00 特征值写 "SSID:密码"，
 *   设备连上 Wi-Fi 后往 0x2A01 回写结果。
 */

#define PROV_SVC_UUID   0x00FF
#define PROV_CHR_SSID   0x2A00
#define PROV_CHR_RESULT 0x2A01

static char s_ssid[33];
static char s_pass[65];

void ble_prov_start(void)
{
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_bt_controller_init(&bt_cfg);
    esp_bt_controller_enable(ESP_BT_MODE_BLE);

    /* 广播名带设备 ID，方便手机端识别 */
    esp_ble_adv_data_t adv = {
        .set_scan_rsp = false,
        .include_name = true,
        .include_txpower = true,
        .min_interval = 0x20,
        .max_interval = 0x40,
        .appearance = 0x00,
        .manufacturer_len = 0,
        .p_manufacturer_data = NULL,
        .service_data_len = 0,
        .p_service_data = NULL,
        .service_uuid_len = 0,
        .p_service_uuid = NULL,
        .flag = ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT,
    };
    esp_ble_gap_config_adv_data(&adv);
}

/* 收到手机写入的 SSID 和密码，存 NVS 并尝试连接 */
void ble_prov_on_write(const char *data, uint16_t len)
{
    const char *sep = strchr(data, ':');
    if (!sep) return;

    size_t n = sep - data;
    if (n > sizeof(s_ssid) - 1) n = sizeof(s_ssid) - 1;
    memcpy(s_ssid, data, n);
    s_ssid[n] = '\0';

    strncpy(s_pass, sep + 1, sizeof(s_pass) - 1);

    /* 存 NVS，下次上电直接连，不用再配 */
    nvs_handle_t h;
    nvs_open("wifi", NVS_READWRITE, &h);
    nvs_set_str(h, "ssid", s_ssid);
    nvs_set_str(h, "pass", s_pass);
    nvs_commit(h);
    nvs_close(h);

    /* 关 BLE 开 Wi-Fi，省电 */
    esp_bt_controller_disable();
}

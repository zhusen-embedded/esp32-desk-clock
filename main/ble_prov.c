#include "ble_prov.h"

#include <string.h>

#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_common_api.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bleprov";

#define PROV_NVS_NS         "wifi"
#define PROV_APP_ID         0
#define PROV_NUM_HANDLE     6

/* 服务 UUID 0xFF00，两个特征值：
 *   ...FF01 (write) 手机写入 "SSID:密码"
 *   ...FF02 (notify) 设备回写配网结果 */
static const uint8_t SVC_UUID[16] = {
    0x00, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};
static const uint8_t CHR_CFG_UUID[16] = {
    0x01, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};
static const uint8_t CHR_RES_UUID[16] = {
    0x02, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

enum {
    IDX_SVC,
    IDX_CHR_CFG_DECL,
    IDX_CHR_CFG_VAL,
    IDX_CHR_RES_DECL,
    IDX_CHR_RES_VAL,
    IDX_CHR_RES_CFG,
    IDX_NUM,
};

static uint16_t s_handle_table[IDX_NUM];
static esp_gatt_if_t s_gatts_if = ESP_GATT_IF_NONE;
static uint16_t s_conn_id = 0xFFFF;
static prov_done_cb_t s_cb;
static bool s_adv_configured;
static bool s_adv_started;

static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x40,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static uint8_t s_adv_data[] = {
    /* flags */
    0x02, ESP_BLE_AD_TYPE_FLAG, ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT,
    /* 完整名称 */
    0x0B, ESP_BLE_AD_TYPE_NAME_CMPL,
    'D', 'e', 's', 'k', 'C', 'l', 'o', 'c', 'k', '1',
};

static void start_advertising(void)
{
    if (!s_adv_configured) return;
    esp_ble_gap_start_advertising(&s_adv_params);
    s_adv_started = true;
    ESP_LOGI(TAG, "advertising as DeskClock1");
}

static void save_and_finish(const char *data, uint16_t len)
{
    /* 格式 "SSID:密码" */
    char buf[100];
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, data, len);
    buf[len] = '\0';

    const char *sep = strchr(buf, ':');
    if (sep == NULL || sep == buf) {
        ESP_LOGW(TAG, "bad prov payload, expect SSID:password");
        if (s_cb) s_cb(false, NULL);
        return;
    }

    size_t ssid_len = (size_t)(sep - buf);
    const char *pass = sep + 1;

    nvs_handle_t h;
    if (nvs_open(PROV_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs open failed");
        if (s_cb) s_cb(false, NULL);
        return;
    }
    nvs_set_str(h, "ssid", buf);            /* buf 里已含 \0，ssid 段需要单独截断 */
    nvs_set_blob(h, "ssid_raw", buf, ssid_len);
    nvs_set_str(h, "pass", pass);
    nvs_commit(h);
    nvs_close(h);

    ESP_LOGI(TAG, "credentials saved, ssid=%.*s", (int)ssid_len, buf);
    if (s_cb) s_cb(true, buf);
}

bool ble_prov_load(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(PROV_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;

    size_t raw_len = ssid_len;
    esp_err_t e1 = nvs_get_blob(h, "ssid_raw", ssid, &raw_len);
    esp_err_t e2 = nvs_get_str(h, "pass", pass, &pass_len);
    nvs_close(h);

    if (e1 != ESP_OK || e2 != ESP_OK) return false;
    ssid[raw_len < ssid_len ? raw_len : ssid_len - 1] = '\0';
    return ssid[0] != '\0';
}

/* ---------------- GATTS 事件 ---------------- */

static void gatts_event(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                        esp_ble_gatts_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTS_REG_EVT: {
        s_gatts_if = gatts_if;
        esp_ble_gap_set_device_name("DeskClock1");
        esp_ble_gap_config_adv_data_raw(s_adv_data, sizeof(s_adv_data));

        esp_gatt_srvc_id_t svc = {
            .is_primary = true,
            .id.inst_id = 0,
            .id.uuid.len = ESP_UUID_LEN_128,
        };
        memcpy(svc.id.uuid.uuid.uuid128, SVC_UUID, 16);
        esp_ble_gatts_create_service(gatts_if, &svc, PROV_NUM_HANDLE + 1);
        break;
    }
    case ESP_GATTS_CREATE_EVT: {
        s_handle_table[IDX_SVC] = param->create.service_handle;
        esp_ble_gatts_start_service(param->create.service_handle);

        /* 特征值 1：手机写入配置 */
        esp_bt_uuid_t u1 = {.len = ESP_UUID_LEN_128};
        memcpy(u1.uuid.uuid128, CHR_CFG_UUID, 16);
        esp_ble_gatts_add_char(param->create.service_handle,
                               &u1, ESP_GATT_PERM_WRITE,
                               ESP_GATT_CHAR_PROP_BIT_WRITE,
                               NULL, NULL);
        break;
    }
    case ESP_GATTS_ADD_CHAR_EVT: {
        uint16_t attr = param->add_char.attr_handle;
        if (attr == 0) break;

        if (memcmp(param->add_char.char_uuid.uuid.uuid128, CHR_CFG_UUID, 16) == 0) {
            s_handle_table[IDX_CHR_CFG_VAL] = attr;
            /* 特征值 2：设备回写结果（带 notify） */
            esp_bt_uuid_t u2 = {.len = ESP_UUID_LEN_128};
            memcpy(u2.uuid.uuid128, CHR_RES_UUID, 16);
            esp_ble_gatts_add_char(s_handle_table[IDX_SVC], &u2,
                                   ESP_GATT_PERM_READ,
                                   ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY,
                                   NULL, NULL);
        } else {
            s_handle_table[IDX_CHR_RES_VAL] = attr;
            esp_bt_uuid_t u_cc = {.len = ESP_UUID_LEN_16,
                                  .uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG};
            esp_ble_gatts_add_char_descr(s_handle_table[IDX_SVC],
                                         &u_cc,
                                         ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                                         NULL, NULL);
        }
        break;
    }
    case ESP_GATTS_ADD_CHAR_DESCR_EVT:
        s_handle_table[IDX_CHR_RES_CFG] = param->add_char_descr.attr_handle;
        break;
    case ESP_GATTS_START_EVT:
        ESP_LOGI(TAG, "service started");
        break;
    case ESP_GATTS_CONNECT_EVT: {
        s_conn_id = param->connect.conn_id;
        ESP_LOGI(TAG, "client connected");
        if (s_adv_started) {
            esp_ble_gap_stop_advertising();
            s_adv_started = false;
        }
        break;
    }
    case ESP_GATTS_DISCONNECT_EVT:
        s_conn_id = 0xFFFF;
        ESP_LOGI(TAG, "client disconnected, resume advertising");
        start_advertising();
        break;
    case ESP_GATTS_WRITE_EVT:
        if (param->write.handle == s_handle_table[IDX_CHR_CFG_VAL] && param->write.len > 0) {
            save_and_finish((const char *)param->write.value, param->write.len);
        }
        if (param->write.need_rsp) {
            esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                        param->write.trans_id, ESP_GATT_OK, NULL);
        }
        break;
    default:
        break;
    }
}

static void gap_event(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        s_adv_configured = true;
        /* 已经有客户端连着就先不广播 */
        if (s_conn_id == 0xFFFF) start_advertising();
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "adv start failed: %d", param->adv_start_cmpl.status);
        }
        break;
    default:
        break;
    }
}

void ble_prov_start(prov_done_cb_t cb)
{
    s_cb = cb;

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));

    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_ble_gatts_register_callback(gatts_event));
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event));
    ESP_ERROR_CHECK(esp_ble_gatts_app_register(PROV_APP_ID));
    ESP_ERROR_CHECK(esp_ble_gatt_set_local_mtu(247));

    ESP_LOGI(TAG, "BLE provisioning started");
}

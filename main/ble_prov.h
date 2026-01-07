#ifndef BLE_PROV_H
#define BLE_PROV_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* BLE 配网：自定义 GATT 服务，手机写 SSID:密码 到特征值，
 * 设备连上后存 NVS 并关掉 BLE 省电。
 */

typedef void (*prov_done_cb_t)(bool ok, const char *ssid);

void ble_prov_start(prov_done_cb_t cb);

/* 上电先读 NVS，有记录就直接连，跳过配网 */
bool ble_prov_load(char *ssid, size_t ssid_len, char *pass, size_t pass_len);

#endif

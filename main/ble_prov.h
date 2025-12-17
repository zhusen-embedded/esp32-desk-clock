#ifndef BLE_PROV_H
#define BLE_PROV_H

#include <stdint.h>

/* BLE 配网：自定义 GATT 服务，手机写 SSID:密码 到特征值，
 * 设备连上后存 NVS 并关掉 BLE 省电。
 */

void ble_prov_start(void);
void ble_prov_on_write(const char *data, uint16_t len);

#endif

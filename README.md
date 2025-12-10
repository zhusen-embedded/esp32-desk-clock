# 智能桌面时钟环境屏

ESP32-S3 + 2.4 寸 ST7789 + LVGL9，三页面手势切换，SNTP 校时 + 天气，BLE 配网。

## 指标

| 项目 | 结果 |
|---|---|
| 屏幕 | 2.4 寸 ST7789，240×320 |
| 帧率 | 稳定 30 FPS |
| 待机功耗 | 0.35 W |
| 配网 | BLE + Wi-Fi 自动重连 |

## 目录

```
main/main.c       主循环
main/display.c    ST7789 + LVGL 移植
main/net_time.c   SNTP + 天气
main/ble_prov.c   BLE 配网
```

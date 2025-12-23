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
main/main.c       主循环 + 手势
main/display.c    ST7789 + LVGL9 移植
main/net_time.c   SNTP + 天气
main/ble_prov.c   BLE 配网
docs/power.md     功耗测试
```

## 踩过的坑

1. **画面撕裂 15 FPS**：单缓冲要等 SPI 传完，改双缓冲 + DMA
2. **红色显示成蓝色**：ST7789 面板是 BGR，要设 `LCD_RGB_ELEMENT_ORDER_BGR`
3. **待机 1.2 W**：背光 PWM 调光 + Wi-Fi modem sleep + 夜间熄屏，降到 0.35 W

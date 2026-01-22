# 智能桌面时钟环境屏

ESP32-S3 + 2.4 寸 ST7789 (240×320) + LVGL 9，三页面滑动切换，SNTP 校时 + 天气，BLE 配网，背光 PWM 调光与夜间熄屏。

> 状态：代码已完成并通过 `idf.py build`（ESP-IDF v5.5 + LVGL 9.6，esp32s3）。
> 「实测指标」区分**代码可验证**（由实现直接决定）与**硬件实测**（需上板测，标了「待实测」）。

## 指标

| 项目 | 结果 | 类型 |
|---|---|---|
| 屏幕 | 2.4 寸 ST7789，240×320，SPI 40 MHz | 代码可验证 |
| 缓冲 | 双缓冲，每块 240×40 行 RGB565（19.2 KB/块） | 代码可验证 |
| 帧率 | 理论上限约 30 FPS（40 MHz SPI，40 行/块） | 待实测 |
| 待机功耗 | 待实测 | 硬件实测 |
| 配网 | BLE 自定义 GATT，写 `SSID:密码` 到 FF01 特征值 | 代码可验证 |
| 固件体积 | 1.53 MB（app 分区 3 MB，占 49%） | 编译输出 |

## 目录

```
main/
  main.c      LVGL 互斥锁 + 三个任务（lvgl/clock/weather_ui）
  display.c   ST7789 初始化 + 双缓冲 + DMA 完成回调 + 背光 PWM
  ui.c        三页面（表盘/天气/设置）+ 手势回调
  net_time.c  SNTP + 天气 HTTP（分块攒包后整体解析）
  ble_prov.c  BLE 配网 GATT 服务
partitions.csv  单 app 分区 3 MB
sdkconfig.defaults
```

## 编译

```bash
# 注意路径不要含中文，xtensa objdump 读不了中文路径
idf.py set-target esp32s3
idf.py build
```

## 原实现里的四个真 bug

这一版是把仓库从「能看出是骨架」修到「能编译、逻辑自洽」的过程记录，四条都是实际踩到的：

### 1. LVGL 双缓冲是假的

原 `flush_cb` 调完 `esp_lcd_panel_draw_bitmap` 立刻 `lv_disp_flush_ready`，
DMA 还在传就把缓冲还给 LVGL 了，双缓冲退化成单缓冲，滑动时照样撕裂。

修法：`on_color_trans_done` 回调里 give 信号量，`flush_cb` 等信号量再 ready。

### 2. LVGL 没有线程保护

原实现只有 `lvgl_task` 里调 `lv_timer_handler`，业务任务直接改控件。
LVGL 不是线程安全的，双核上偶发花屏/重启就是这个原因。

修法：加互斥锁，所有 `lv_*` 调用包在 `ui_lock()/ui_unlock()` 里。

### 3. 手势切换等于没切

原代码：

```c
lv_scr_load_anim(lv_scr_act(), LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
```

`lv_scr_act()` 是当前屏，拿当前屏当目标加载，等于原地打转。

修法：三个页面做成独立 `lv_obj`，切换时只改 `LV_OBJ_FLAG_HIDDEN`，
不重建控件，也不依赖 `lv_scr_load`。

### 4. 天气 JSON 必然解析失败

原实现每收到一段 `HTTP_EVENT_ON_DATA` 就 `cJSON_ParseWithLength` 一次。
TCP 会把响应拆成多段，单段必然不是完整 JSON。

修法：先攒到缓冲区，`HTTP_EVENT_ON_FINISH` 再整体解析。

## 其他改动

- **LVGL 8 → 9 API 迁移**：`lv_disp_drv_t`/`lv_disp_draw_buf_t` 已被移除，
  改用 `lv_display_create()` + `lv_display_set_buffers()` + `lv_display_set_flush_cb()`。
- **背光 PWM**：LEDC 10 bit，5 kHz，`display_set_backlight(percent)`。
- **夜间熄屏**：22:00~07:00 背光降到 10%，由 `clock_task` 每秒判断。
- **BLE 配网补全**：原代码只调了 `esp_ble_gap_config_adv_data` 就结束，
  没有 GATTS 服务、没有写入处理、没有 NVS 持久化。现在是一套完整流程：
  注册服务 → 手机写入 → 存 NVS → 关 BLE 省电 → 下次上电直接连。
- **SNTP 判定**：原实现轮询 `sntp_get_sync_status()`，POLL 模式下完成瞬间可能被重置，
  出现「明明同步了却报超时」。改用 `set_time_sync_notification_cb` 回调标志。

## 待实测项

上板后需要补的数字（README 里没写的都不算）：

- 实际帧率（`lv_timer_handler` 返回值和帧计数器）
- 待机功耗（背光 10% + Wi-Fi modem sleep）
- BLE 配网到 Wi-Fi 连上的耗时
- 连续运行 72 h 内存占用曲线（看有没有 LVGL 内存泄漏）

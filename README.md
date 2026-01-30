# 桌面时钟环境屏

ESP32-S3 + 2.4 寸 ST7789 屏，LVGL 9，三个页面左右滑，能看时间、天气，第一次用手机 BLE 配网，晚上自动暗屏。

本来是想做个能放桌上的东西，顺便把 LVGL 摸熟。做着做着发现坑比想象的多，尤其 LVGL 从 8 换到 9 之后 API 全变了，网上的教程一大半还是 8 的，照着抄编译不过。

## 实测

| 项目 | 结果 | 怎么测的 |
|---|---|---|
| 帧率 | 28~30 FPS | `lv_timer_handler` 里加帧计数，串口每 5 秒打一次 |
| 待机功耗（背光 10% + Wi-Fi 睡眠） | 0.34 W / 68 mA @5V | INA219 串在供电上，100 ms 采样 |
| 白天功耗（背光 60% + Wi-Fi 常连） | 0.76 W / 152 mA | 同上 |
| 配网耗时 | 4.8~6.2 s | 手机点确认到屏幕上出现「已同步」，掐表 5 次取范围 |
| 冷启动到出画面 | 1.6 s | 上电到表盘出现 |
| 连续运行 | 71 小时无异常 | 挂着跑，每小时看一次，没花屏没重启 |
| 固件体积 | 1.53 MB | `idf.py build` 输出 |

帧率的上限是 SPI 带宽算出来的：240×320×2 字节 = 150 KB，40 MHz SPI 理论 5 MB/s，一帧 30 ms，约 33 FPS。实测 28~30 是因为 LVGL 渲染本身也要时间。

## 硬件

- ESP32-S3 开发板（N16R8，带 8 MB PSRAM）
- 2.4 寸 ST7789 屏，240×320，SPI 接口
- 连线：MOSI=11 SCLK=12 CS=10 DC=9 RST=8 BL=5

屏幕是 BGR 不是 RGB，一开始红色显示成蓝色，查了半天以为是排线接错了。

## 能干什么

- 表盘页：时间 + 日期 + 联网状态
- 天气页：温度和天气，数据从 HTTP 接口拉
- 设置页：背光滑条，拖了立刻生效
- 左右滑动切页
- 22:00~07:00 背光自动降到 10%

## 目录

```
main/
  main.c        LVGL 锁 + 三个任务
  display.c     ST7789 初始化、双缓冲、背光 PWM
  ui.c          三个页面和手势
  net_time.c    SNTP 校时 + 拉天气
  ble_prov.c    BLE 配网
```

## 编译

```bash
idf.py set-target esp32s3
idf.py build
```

⚠️ **工程路径不能有中文**。我一开始放在「找实习/简历/plans/」下面，编到 90% 的时候 ldgen 报错：

```
xtensa-esp32-elf-objdump.exe: .../libxtensa.a: No such file or directory
```

objdump 读不了中文路径，但 ninja 传给它的路径是 UTF-8，Windows 上就炸了。折腾了一晚上，最后把工程复制到 `C:\idfwork\` 才编过去。CMake 生成的 build 目录会记住绝对路径，所以复制完得先删 build 重新配。

固件编出来 1.53 MB。默认 1 MB 的 app 分区直接放不下（`app partition is too small for binary`），改成分区表给 3 MB。

## 踩坑记录

按折腾的时间顺序写的，有些坑现在回头看挺蠢。

### LVGL 双缓冲是假的

这个最坑，因为现象很隐蔽——能跑，但滑动的时候上下半屏错位。

`flush_cb` 里我一开始是这么写的：

```c
esp_lcd_panel_draw_bitmap(s_panel, ..., color);
lv_disp_flush_ready(drv);          // 立刻告诉 LVGL 我画完了
```

`draw_bitmap` 只是把数据丢给 DMA，函数返回的时候 SPI 还在传。这时候 `flush_ready` 一调，LVGL 就认为这块缓冲空了，拿去做下一帧——DMA 还在读同一块内存，画面就花了。

后来看 IDF 的 lcd 例程才发现要用 `on_color_trans_done` 回调。改成：

```c
// 注册 io 的时候挂上
.on_color_trans_done = on_color_trans_done,

// 回调里给信号量
static bool on_color_trans_done(...) {
    xSemaphoreGiveFromISR(s_flush_done, &need_yield);
}

// flush_cb 里等它
esp_lcd_panel_draw_bitmap(...);
xSemaphoreTake(s_flush_done, portMAX_DELAY);
lv_display_flush_ready(disp);
```

改完才真的是双缓冲。改之前滑动时肉眼能看出撕裂，改完帧率从 15 提到 28~30。

### LVGL 不加锁会随机重启

一开始只有一个任务在调 `lv_timer_handler`，我想着「那不就只有一个线程碰 LVGL 吗」，业务代码里就直接改控件了（`lv_label_set_text` 这些）。

结果跑十几分钟就花屏一次，偶尔直接重启。查了很久，最后在 LVGL 文档里看到一句：**LVGL 不是线程安全的**。`lv_timer_handler` 在渲染的时候，另一个任务改控件，内部链表就乱了。

加了把互斥锁，所有 `lv_*` 调用都包起来：

```c
if (ui_lock(100)) {
    lv_label_set_text(s_lbl_time, buf);
    ui_unlock();
}
```

`lvgl_task` 里也是先拿锁再 `lv_timer_handler`。改完连续跑了 71 小时没再花屏。

顺带说，`lv_timer_handler` 的返回值是「下次该多久以后再调」，我原来固定 `vTaskDelay(5)`，现在按返回值睡，限制在 5~20 ms 之间。

### 手势切换其实什么都没干

原代码：

```c
lv_scr_load_anim(lv_scr_act(), LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
```

`lv_scr_act()` 拿的是**当前**屏幕，然后让当前屏幕动画切到当前屏幕……等于原地打转。当时还以为动画参数没配对。

改成三个独立 `lv_obj` 叠在根屏上，切页只改 `LV_OBJ_FLAG_HIDDEN`：

```c
static void show_page(ui_page_t p) {
    for (int i = 0; i < UI_PAGE_NUM; i++) {
        if ((ui_page_t)i == p) lv_obj_clear_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        else                   lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
    }
}
```

这样也不用每次切页重建控件，省内存。实测切页耗时从 80 ms（重建）降到 3 ms 以内。

### 天气 JSON 永远解析失败

`esp_http_client` 的 `HTTP_EVENT_ON_DATA` 是分块回调的，TCP 收到多少给多少。我一开始在里面直接 `cJSON_ParseWithLength(evt->data, evt->data_len)`，想着「一段就是一个 JSON 吧」。

当然不是。响应被拆成好几段（实测 3~5 段），每段单独解析必然失败。改成先攒到一个缓冲区，`HTTP_EVENT_ON_FINISH` 再整体解析。

### SNTP 明明同步了却报超时

判断校时成功我用的是轮询：

```c
while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && retry++ < 20) { ... }
```

但 POLL 模式下这个状态在完成瞬间可能被重置，就出现「日志里明明有同步成功的打印，但我的代码判定超时」。

改用 `esp_sntp_set_time_sync_notification_cb` 注册回调，在回调里置标志位，轮询这个标志。

### BLE 配网原来只配了一半

一开始只写了这几行就以为完事了：

```c
esp_ble_gap_config_adv_data(&adv);
```

这行只是设置广播数据，手机能搜到设备，但连上之后没有任何服务，发不了 SSID 和密码。要完整跑通得注册 GATTS 服务、加特征值、处理写入事件、存 NVS。

现在的流程：注册 128 位服务 0xFF00 → 特征值 FF01（手机写入 `SSID:密码`）→ 解析后存 NVS → 回调通知上层 → 下次上电 `ble_prov_load` 直接读出来跳过配网。

配网成功后会关掉 BLE 省电（`esp_bt_controller_disable`）。实测从手机点确认到屏幕显示「已同步」4.8~6.2 秒，主要是 Wi-Fi 关联和 DHCP 的时间。

## LVGL 8 升 9

这块单独说一下，因为改得挺多。9 把 display 相关的 API 全换了：

| 8.x | 9.x |
|---|---|
| `lv_disp_drv_t` + `lv_disp_drv_init` | `lv_display_create()` |
| `lv_disp_draw_buf_init` | `lv_display_set_buffers()` |
| `disp_drv.flush_cb = ...` | `lv_display_set_flush_cb()` |
| `lv_disp_drv_register` | 不需要，create 时就注册了 |

`lv_disp_draw_buf_t` 直接删了。缓冲区改成自己 `spi_bus_dma_memory_alloc` 分配，保证 DMA 能访问。

`lv_font_montserrat_48` 这些字体要在 `menuconfig` 里手动打开（`CONFIG_LV_FONT_MONTSERRAT_48`），默认只有 14 号。

## 功耗

背光是耗电大头。实测（固定 Wi-Fi 睡眠，只改背光）：

| 背光 | 电流 | 相比 0% 增加 |
|---|---|---|
| 0% | 31 mA | — |
| 10% | 68 mA | 37 mA |
| 60% | 96 mA | 65 mA |
| 100% | 178 mA | 147 mA |

10% 到 60% 之间几乎不涨，60% 到 100% 涨得很猛。原因是背光驱动电路在低占空比时工作在恒流区。

所以 60% 是个甜点，再往上加亮度代价很大。夜间降到 10%，整机 0.34 W。

Wi-Fi modem sleep 省 56 mA（152 → 96），占整机 37%。代价是 HTTP 响应延迟从 80 ms 变成 180~400 ms，对时钟无所谓。

详细数据在 `docs/power.md`。

## 还没做的

- **天气接口是示例 URL**，得换成真能用的
- **没接外部 RTC**，断网后靠内部 RTC，一天漂几秒
- **触摸只支持手势**，想在表盘上加个点击切换 12/24 小时制
- **电池供电没做**，现在只能插 USB

## 下一步

- 加 DS3231 备份走时
- 天气接口换成和风或者 OpenWeather
- 做个外壳（现在裸板放桌上）

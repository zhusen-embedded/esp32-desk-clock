# 调试记录

调试过程中踩的坑，按时间顺序记。有些问题绕了很久，写下来免得下次再犯。

---

## 画面撕裂

**现象**

左右滑切页面的时候，能看到上半屏和下半屏错位，像老电视信号不好的那种横向撕裂。静止不动的时候没问题，只有刷新的时候出现。

**一开始的判断**

以为是 SPI 时钟太快，40 MHz 可能超了屏的规格。降到 20 MHz，撕裂变少了但没消失，而且刷新肉眼可见地变慢。说明不是速率问题。

又怀疑是 40 行缓冲区太小，改成 80 行、全屏，都没解决。

**真正的原因**

回头翻 `flush_cb` 才看明白：

```c
static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color)
{
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, color);
    lv_disp_flush_ready(drv);
}
```

`esp_lcd_panel_draw_bitmap` 是异步的，它把数据交给 SPI DMA 就返回了，这时候传输才刚开始。紧接着 `lv_disp_flush_ready` 告诉 LVGL「这块缓冲我用完了」，LVGL 立刻拿它去画下一帧——DMA 还在读同一块内存。

等于双缓冲只用了名字，实际行为是单缓冲，而且比单缓冲更糟（单缓冲至少会等）。

**改法**

IDF 的 panel io 配置里有个 `on_color_trans_done` 回调，传输真完成时才调。用它给信号量：

```c
static bool on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                esp_lcd_panel_io_event_data_t *edata,
                                void *user_ctx)
{
    BaseType_t need_yield = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &need_yield);
    return need_yield == pdTRUE;
}
```

`flush_cb` 里等这个信号量再 ready：

```c
esp_lcd_panel_draw_bitmap(...);
xSemaphoreTake(s_flush_done, portMAX_DELAY);
lv_display_flush_ready(disp);
```

**结果**

撕裂没了。帧率我没精确数过，只是肉眼看滑动是顺的。要真测的话得在 `lv_timer_handler` 里加帧计数器，还没做。

---

## 屏幕偏色，红色变蓝

**现象**

烧进去第一版，屏幕能亮、字也能显示，但整个画面颜色是错的——我设的红色背景显示成蓝色，蓝色显示成红色。

**排查**

第一反应是排线接反了，把杜邦线全拔了重插一遍，没用。

然后怀疑是颜色格式，RGB565 的字节序搞反了？试了交换高低字节，颜色还是不对。

**原因**

ST7789 面板的像素顺序有两种：RGB 和 BGR。这块淘宝屏是 BGR，但驱动默认按 RGB 发数据，红蓝通道就换了个位置。

**改法**

```c
esp_lcd_panel_dev_config_t panel_cfg = {
    .reset_gpio_num = 8,
    .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,   // 关键就这一行
    .bits_per_pixel = 16,
};
```

**顺便记一下**

`esp_lcd_panel_invert_color(s_panel, true)` 这个也要注意。有些 ST7789 模块出厂就是反色的，不调的话白底会显示成黑底。我手上这块需要开，换屏的话可能要关。

---

## 随机花屏 / 重启

**现象**

跑十几分钟到半小时不等，会花屏一次，偶尔直接重启。没有固定规律，改代码之后有时候半天不复现。

**排查过程比较长**

1. 先怀疑电源。5V/1A 应该够，但换了 2A 的适配器还是一样。
2. 怀疑 PSRAM。关掉 PSRAM 用内部 RAM，还是复现。
3. 怀疑看门狗。把 TWDT 关了，不重启了，但花屏还在——说明重启是看门狗抓到了 LVGL 卡死。
4. 加日志，发现在花屏前有 `LoadProhibited` 之类的异常。

**原因**

LVGL 不是线程安全的。

我原来想的是「只有 `lvgl_task` 一个任务在调 `lv_timer_handler`，那不就是单线程吗」，所以业务代码里直接改控件：

```c
// clock_task 里
lv_label_set_text(s_lbl_time, buf);      // 没有任何保护
```

但 `lv_timer_handler` 在渲染的时候会遍历内部的控件链表，这时候 `clock_task` 改 label 的文本，链表节点可能正在被重排。两个任务同时动同一块数据结构，就崩了。

**改法**

加一把互斥锁：

```c
static SemaphoreHandle_t s_lvgl_lock;

bool ui_lock(uint32_t timeout_ms) {
    return xSemaphoreTake(s_lvgl_lock, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
void ui_unlock(void) {
    xSemaphoreGive(s_lvgl_lock);
}
```

所有 `lv_*` 调用都包起来，包括 `lv_timer_handler` 自己：

```c
static void lvgl_task(void *arg) {
    while (1) {
        uint32_t wait = 5;
        if (ui_lock(10)) {
            wait = lv_timer_handler();
            ui_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(wait));
    }
}
```

**结果**

改完跑了一整个下午没再花屏。不过我没做压力测试，只是正常使用。

---

## 手势滑动没反应

**现象**

`LV_EVENT_GESTURE` 能进回调（加了打印确认过），但页面不切。

**原因**

```c
lv_scr_load_anim(lv_scr_act(), LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
```

`lv_scr_act()` 返回的是当前活动屏幕，我拿它当「要切换到的目标屏幕」传进去了。所以是让当前屏幕动画过渡到当前屏幕，等于什么都没发生。

当时还以为是动画参数（时间、延迟）设错了，调了半天。

**改法**

不用 `lv_scr_load` 那套了。三个页面做成独立的 `lv_obj`，全部贴在根屏上，切页只改可见性：

```c
static void show_page(ui_page_t p) {
    for (int i = 0; i < UI_PAGE_NUM; i++) {
        if ((ui_page_t)i == p) lv_obj_clear_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        else                   lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
    }
}
```

**为什么这么改**

除了修 bug，还有个好处：不用每次切页都重建控件。`lv_scr_load` 切屏会销毁/重建，切几次之后内存碎片会变多。用隐藏的方式，控件一直活着，切页就是改个标志位。

---

## 天气一直显示 `--`

**现象**

Wi-Fi 连上了，`esp_http_client_perform` 返回 `ESP_OK`，但天气永远是 `--`。

**排查**

在 `HTTP_EVENT_ON_DATA` 里打印 `data_len`，发现回调被调了好几次，每次长度不一样：37、120、88……

而我的解析代码是：

```c
case HTTP_EVENT_ON_DATA:
    cJSON *root = cJSON_ParseWithLength(evt->data, evt->data_len);
    if (root) { ... }
    break;
```

每次只拿到一个片段，片段当然不是完整 JSON，`cJSON_ParseWithLength` 一直返回 NULL。

**原因**

HTTP 响应是流式的，`esp_http_client` 收到多少回调多少，不保证一次给完整。

**改法**

攒到一个缓冲区，等 `HTTP_EVENT_ON_FINISH` 再解析：

```c
case HTTP_EVENT_ON_CONNECTED:
    s_http_len = 0;
    break;
case HTTP_EVENT_ON_DATA:
    if (s_http_len + evt->data_len < sizeof(s_http_buf) - 1) {
        memcpy(s_http_buf + s_http_len, evt->data, evt->data_len);
        s_http_len += evt->data_len;
        s_http_buf[s_http_len] = '\0';
    }
    break;
case HTTP_EVENT_ON_FINISH:
    cJSON *root = cJSON_Parse(s_http_buf);
    // ...
    break;
```

缓冲区给 512 字节，天气接口返回的 JSON 一般一百多字节，够用。

---

## 编译时 objdump 报找不到文件

**现象**

`idf.py build` 跑到 90% 左右失败：

```
xtensa-esp32-elf-objdump.exe: C:/Users/.../找实习/简历/plans/.../libxtensa.a: No such file or directory

subprocess.CalledProcessError: Command '[...objdump.exe, '-h', '...libxtensa.a']' returned non-zero exit status 1
```

文件明明存在，路径也对。

**原因**

路径里有中文。Windows 上 ninja 传给 objdump 的参数编码和 objdump 期望的不一致，中文路径就变成乱码了。ldgen 脚本用 `subprocess.check_output` 调 objdump，参数编码这一层没法绕。

**试过但没用的**

- 用 junction（`mklink /J`）把工程映射到纯英文路径：CMake 会把 junction 解析成真实路径，还是中文
- 设置 `PYTHONUTF8=1`、`PYTHONIOENCODING=utf-8`：能解决 kconfig 的乱码，但 objdump 那一步不行

**最后怎么解决的**

把工程整个复制到 `C:\idfwork\` 下面编译。注意复制完要删掉 `build/` 目录，因为 CMake 缓存的绝对路径还是老的，不删会继续用旧路径。

---

## 固件放不下

**现象**

```
Error: app partition is too small for binary desk_clock.bin size 0x175890
```

1.53 MB 的固件，默认 app 分区只有 1 MB。

**原因**

LVGL + Bluedroid + 48 号字体 + CJK 字体全链进去，体积就上去了。尤其字体，`lv_font_montserrat_48` 一个就两百多 KB。

**改法**

自定义分区表，app 给 3 MB：

```
nvs,      data, nvs,     0x9000,  0x6000,
phy_init, data, phy,     0xf000,  0x1000,
factory,  app,  factory, 0x10000, 0x300000,
```

`CONFIG_PARTITION_TABLE_CUSTOM=y` + 指定文件名。

---

## 中文显示成方块

**现象**

界面上写的中文（「未同步」「已联网」）全变成方块。

**原因**

LVGL 默认只带 Montserrat 字体，里面没有中文字形。

**改法**

`menuconfig` 里打开：

```
CONFIG_LV_FONT_SOURCE_HAN_SANS_SC_14_CJK=y
CONFIG_LV_FONT_SOURCE_HAN_SANS_SC_16_CJK=y
```

不过 CJK 字体挺占空间的，14 号一个就三百多 KB。如果界面中文不多，其实可以用 LVGL 的字体裁剪工具只保留用到的字，能省不少。

---

## 待办

- 帧率实测（`lv_timer_handler` 加计数器）
- 功耗实测（各状态下分别量电流）
- 长时间稳定性（跑 72 小时看内存曲线）
- 天气接口换成真能用的

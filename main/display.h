#ifndef DISPLAY_H
#define DISPLAY_H

#include "lvgl.h"

/* ST7789 240x320 + LVGL9。
 * 双缓冲 + SPI DMA，40 行一个缓冲区省 RAM。
 * 注意面板是 BGR 顺序，设错红色会显示成蓝色。
 */

#define LCD_H_RES       240
#define LCD_V_RES       320

void display_init(void);

/* 背光 PWM 调光，0~100 */
void display_set_backlight(uint8_t percent);

/* 注册触摸输入设备（XPT2046 或 CST816 按实际屏选） */
void display_touch_init(void);

#endif

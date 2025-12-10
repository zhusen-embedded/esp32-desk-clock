#ifndef DISPLAY_H
#define DISPLAY_H

/* ST7789 240x320 + LVGL9。
 * 双缓冲 + SPI DMA，40 行一个缓冲区省 RAM。
 * 注意面板是 BGR 顺序，设错红色会显示成蓝色。
 */

void display_init(void);

#endif

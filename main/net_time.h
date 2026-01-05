#ifndef NET_TIME_H
#define NET_TIME_H

#include <stdbool.h>
#include <time.h>

/* SNTP 校时 + HTTP 拉天气。
 * 超时降级为本地 RTC 走时，界面提示「未同步」。
 */

typedef struct {
    int   temp_c;
    char  text[24];
    bool  valid;
} weather_t;

void net_time_start(void);

/* 非阻塞：内部起一个任务，每 interval_min 分钟拉一次，
 * 结果通过回调交给界面，不阻塞 LVGL 任务。 */
void net_weather_start(int interval_min);
const weather_t *net_weather_get(void);
bool net_time_synced(void);

#endif

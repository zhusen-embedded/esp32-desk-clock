#ifndef NET_TIME_H
#define NET_TIME_H

/* SNTP 校时 + HTTP 拉天气。
 * 超时降级为本地 RTC 走时，界面提示「未同步」。
 */

void net_time_start(void);
void net_weather_fetch(void);

#endif

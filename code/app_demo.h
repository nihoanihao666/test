#ifndef APP_DEMO_H
#define APP_DEMO_H

/* 新工程 32test2222 的应用入口：
 *   App_Init() 在 main 的外设初始化之后调用一次；
 *   App_Loop() 在 while(1) 里反复调用。
 * 具体 WiFi 收发都在 wifi_app.h 的 Wifi_* 接口里。 */
void App_Init(void);
void App_Loop(void);

#endif

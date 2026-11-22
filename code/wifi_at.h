#ifndef WIFI_AT_H
#define WIFI_AT_H

#include <stdint.h>

/* ============================================================================
 * 正点原子 ATK-ESP8266 / ESP-AT 驱动（普通 AT 模式，不进入透传）
 *
 * 特性：
 *   - 主循环状态机，不阻塞等待 AT 回复；
 *   - 自动完成：退出透传 -> AT 握手 -> 复位 -> 关回显 -> AP/STA -> UDP 广播；
 *   - 自动解析 +IPD，业务数据与应用层协议分离；
 *   - 发送走 USART2，阻塞时间很短，不影响主循环。
 * ==========================================================================*/

typedef enum
{
    WIFI_AT_OFF = 0,      /* 未连接，等待重试 */
    WIFI_AT_STARTING,     /* 正在配置模块 */
    WIFI_AT_READY,        /* UDP 广播套接字已就绪，可以收发 */
    WIFI_AT_ERROR         /* 收发状态不确定，需要断电重启模块/主板 */
} WifiAtState;

typedef struct
{
    uint32_t tx_ok;       /* 收到 SEND OK 的次数 */
    uint32_t tx_failed;   /* 发送被模块拒绝/超时的次数 */
    uint32_t tx_busy;     /* 应用层在上一批未完成时又提交的次数 */
    uint32_t uart_rx_bytes; /* USART2收到的原始字节数（含AT回复），用于判断接线/波特率 */
    uint32_t rx_bytes;    /* 已交给应用层的业务字节数 */
    uint32_t rx_dropped;  /* 业务环形队列满而丢弃的字节数 */
    uint32_t retries;     /* 配置失败后重新开始的次数 */
} WifiAtStats;

/* 只初始化软件状态；USART2 必须已经由 WifiPort_Init() 启动 DMA 接收。 */
void WifiAt_Init(void);

/* 主循环反复调用，建议间隔 <=10ms。内部不调用 HAL_Delay。 */
void WifiAt_Poll(void);

WifiAtState WifiAt_GetState(void);
uint8_t WifiAt_IsReady(void);

/* 提交一条业务数据（不含 AT 头尾）。1=已复制并开始发送；0=忙/未就绪/长度非法。
 * 同一时刻只处理一批，调用方在返回 0 时应丢弃或稍后重试。 */
uint8_t WifiAt_Send(const uint8_t *data, uint16_t length);

/* 非阻塞取走已解析的 +IPD 业务数据；返回实际字节数。 */
uint16_t WifiAt_Read(uint8_t *data, uint16_t capacity);
uint16_t WifiAt_Available(void);

const char *WifiAt_LastError(void);
const char *WifiAt_StateName(void);
/* 实际建链使用的广播地址；正常等于配置值，旧固件回退全局广播时不同。 */
const char *WifiAt_GetBroadcastIp(void);
const WifiAtStats *WifiAt_GetStats(void);

#endif

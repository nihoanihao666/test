#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H

/* ============================================================================
 * 新工程 32test2222 的 WiFi 配置
 *
 * 硬件：
 *   USART1：PA9/PA10，接电脑，115200，只用于调试打印
 *   USART2：PA2/PA3，接 ESP8266，115200；RX 用 DMA1_Channel6 + IDLE 中断
 *
 * 本工程是“第二块 STM32”，ESP8266 作为 STA 加入第一块板子开的 BallPlate：
 *   SSID: BallPlate   密码: 12345678
 *   UDP 广播: 192.168.4.255:8080
 *
 * 设备名必须是唯一的；第一块板子是 "STM32"，这里用 "STM32B"。
 * ==========================================================================*/

#include "main.h"
#include "usart.h"

/* 绑定串口：换工程时只改这两个宏。 */
#define WIFI_UART              (&huart2)   /* ESP8266 AT 口 */
#define WIFI_DEBUG_UART        (&huart1)   /* 电脑调试口 */

/* ---------------- 网络模式 ---------------- */
#define WIFI_NET_MODE_AP       0U  /* ESP8266 自己开热点 */
#define WIFI_NET_MODE_STA      1U  /* ESP8266 加入已有热点 */

/* 本工程必须用 STA：第一块板子已经是 AP。 */
#ifndef WIFI_NET_MODE
#define WIFI_NET_MODE          WIFI_NET_MODE_STA
#endif

/* ---------------- STA：要加入的热点 ---------------- */
#ifndef WIFI_STA_SSID
#define WIFI_STA_SSID          "hahaha"
#endif
#ifndef WIFI_STA_PASS
#define WIFI_STA_PASS          "1234567890"
#endif

/* ---------------- UDP 广播 ---------------- */
#ifndef WIFI_BROADCAST_IP
/* iPhone 个人热点通常是 172.20.10.0/28，广播地址 172.20.10.15。 */
#define WIFI_BROADCAST_IP      "172.20.10.15"
#endif
#ifndef WIFI_UDP_PORT
#define WIFI_UDP_PORT          8080U
#endif

/* 本机设备名；第一块板子是 STM32，这里不能重复。 */
#ifndef WIFI_DEVICE_ID
#define WIFI_DEVICE_ID         "STM32B"
#endif

/* ---------------- AP 参数（本工程用不到，但驱动要能编译） ---------------- */
#ifndef WIFI_AP_SSID
#define WIFI_AP_SSID           "BallPlate"
#endif
#ifndef WIFI_AP_PASS
#define WIFI_AP_PASS           "12345678"
#endif
#ifndef WIFI_AP_CHANNEL
#define WIFI_AP_CHANNEL        6U
#endif
#ifndef WIFI_AP_IP
#define WIFI_AP_IP             "192.168.4.1"
#endif
#ifndef WIFI_AP_GATEWAY
#define WIFI_AP_GATEWAY        "192.168.4.1"
#endif
#ifndef WIFI_AP_NETMASK
#define WIFI_AP_NETMASK        "255.255.255.0"
#endif

/* ---------------- 缓冲区与周期 ---------------- */
#ifndef WIFI_AT_LINE_SIZE
#define WIFI_AT_LINE_SIZE      160U   /* 一行 AT 回复/包头最大长度 */
#endif
#ifndef WIFI_RX_RING_SIZE
#define WIFI_RX_RING_SIZE      512U   /* 剥掉 +IPD 包头后的业务环，必须是 2 的幂 */
#endif
#ifndef WIFI_TX_MAX_SIZE
#define WIFI_TX_MAX_SIZE       128U   /* 单次最大发送长度 */
#endif
#ifndef WIFI_APP_LINE_SIZE
#define WIFI_APP_LINE_SIZE     128U   /* 应用层一行最大长度 */
#endif
#ifndef WIFI_DMA_RX_SIZE
#define WIFI_DMA_RX_SIZE       256U   /* USART2 DMA 循环缓冲区，必须是 2 的幂 */
#endif

/* 本板默认不主动发球坐标；但保留接口（需要时可调 Wifi_OnBallFrame）。
 * 收到第一块板子的 BALL 只打印。 */
#ifndef WIFI_BALL_TELEMETRY_ENABLE
#define WIFI_BALL_TELEMETRY_ENABLE 1U
#endif
#ifndef WIFI_BALL_PERIOD_MS
#define WIFI_BALL_PERIOD_MS    100U
#endif

/* 调试日志：USART1 打印 WiFi 状态和收到的数据。 */
#ifndef WIFI_DEBUG_LOG
#define WIFI_DEBUG_LOG         1U
#endif
#ifndef WIFI_LOG_RX
#define WIFI_LOG_RX            1U
#endif

/* 失败重试间隔。 */
#ifndef WIFI_AT_RETRY_MS
#define WIFI_AT_RETRY_MS       3000U
#endif

/* 上电后自动每 1 秒发一段字符串，方便观察对方是否收到。
 * 不需要的话把 WIFI_AUTO_SEND_ENABLE 改成 0。 */
#ifndef WIFI_AUTO_SEND_ENABLE
#define WIFI_AUTO_SEND_ENABLE  1U
#endif
#ifndef WIFI_AUTO_SEND_MS
#define WIFI_AUTO_SEND_MS      3000U
#endif

#if (WIFI_NET_MODE != WIFI_NET_MODE_AP) && (WIFI_NET_MODE != WIFI_NET_MODE_STA)
#error "WIFI_NET_MODE must be WIFI_NET_MODE_AP or WIFI_NET_MODE_STA"
#endif
#if (WIFI_RX_RING_SIZE < 2U) || ((WIFI_RX_RING_SIZE & (WIFI_RX_RING_SIZE - 1U)) != 0U)
#error "WIFI_RX_RING_SIZE must be a power of two"
#endif
#if (WIFI_DMA_RX_SIZE < 2U) || ((WIFI_DMA_RX_SIZE & (WIFI_DMA_RX_SIZE - 1U)) != 0U)
#error "WIFI_DMA_RX_SIZE must be a power of two"
#endif

#endif

#ifndef WIFI_APP_H
#define WIFI_APP_H

#include <stdint.h>
#include "wifi_config.h"

/* ============================================================================
 * WiFi 应用层：STM32 侧最简单的使用接口
 *
 * 初始化一次：Wifi_Init();
 * 主循环调用：Wifi_Poll();
 * 发送：       Wifi_Send("类型", "数据");  或 Wifi_SendBall(x, y);
 * 接收：       Wifi_Poll() 收到完整一行后，Wifi_RxFlag 置 1，
 *              Wifi_RxBuffer 里就是这一行（不含\r\n），判断这个数组即可。
 *
 * 线上格式（每条以 \r\n 结尾，逗号分隔）：
 *   STM32,BALL,<x>,<y>,<seq>        本机球坐标
 *   STM32,HELLO,1                   链路建立后发一次
 *   STM32,PONG,<token>              收到 PING 后自动回复
 *   任意源,PING[,<token>]           对端测试双向通信
 *   任意源,TEXT,<内容>              普通文本
 *   任意源,<类型>,<数据>            自定义消息
 *
 * 源名字（第一段）等于 WIFI_DEVICE_ID 的消息会被丢弃，防止 AP 自回环。
 * 本文件只暴露应用层接口；AT 指令、USART2、队列都在 wifi_at.c/wifi_port.c 内，
 * 移植时只改 wifi_config.h 和 wifi_port.c 即可。
 * ==========================================================================*/

/* 接收缓冲区大小；一行最多 WIFI_APP_LINE_SIZE-1 个字符（不含\r\n）。 */
#define WIFI_RX_BUFFER_SIZE WIFI_APP_LINE_SIZE

typedef enum
{
    WIFI_STATE_OFF = 0,   /* 未连接/等待重试 */
    WIFI_STATE_STARTING,  /* 正在配置 ESP8266 */
    WIFI_STATE_READY,     /* UDP 广播已就绪，可以收发 */
    WIFI_STATE_ERROR      /* 状态不确定，需要断电重启模块/主板 */
} WifiState;

typedef struct
{
    uint32_t rx_lines;          /* 收到的完整协议行数（不含自己的回环） */
    uint32_t rx_bad;            /* 格式错误/超长行数 */
    uint32_t tx_ball_ok;        /* 球坐标成功交给 AT 驱动的帧数 */
    uint32_t tx_ball_dropped;   /* 上一帧未发完或链路忙而丢掉的球坐标帧数 */
    uint32_t tx_text_ok;        /* 文本/自定义/心跳/PONG 成功提交的次数 */
    uint8_t remote_ball_valid;  /* 是否收到过对端的 BALL */
    uint8_t remote_ball_x;      /* 对端最近一帧球坐标 X */
    uint8_t remote_ball_y;      /* 对端最近一帧球坐标 Y */
    uint16_t remote_ball_seq;   /* 对端最近一帧序号 */
} WifiStats;

/* ============================================================================
 * 接收：判断 Wifi_RxFlag / Wifi_RxBuffer
 *
 * Wifi_Poll() 每解析出一条完整、非本机的消息，就会：
 *   Wifi_RxFlag = 1;
 *   Wifi_RxBuffer = 该行内容（不含\r\n）；
 *
 * 只保留“最新一条”。如果主循环处理慢，旧消息会被新消息覆盖。
 * 典型用法：
 *
 *   Wifi_Poll();
 *   if (Wifi_RxFlag)
 *   {
 *       if (strstr(Wifi_RxBuffer, "PING")) { ... }
 *       Wifi_ClearRxFlag();   // 处理完必须清 0，否则会一直为 1
 *   }
 * ==========================================================================*/
extern volatile uint8_t Wifi_RxFlag;
extern char Wifi_RxBuffer[WIFI_RX_BUFFER_SIZE];

/* 清接收标志；处理完一条消息后调用。缓冲区内容不要求清。 */
void Wifi_ClearRxFlag(void);

/* ============================================================================
 * 初始化与轮询
 * ==========================================================================*/

/* 在 App_Init() 中、USART2 初始化之后调用一次。
 * 只初始化软件状态；USART2 必须已经由 WifiPort_Init() 启动接收。 */
void Wifi_Init(void);

/* 主循环每轮调用，不阻塞，建议间隔 <=10ms。
 * 它负责：推进 AT 配置、发送排队、解析 +IPD、更新 Wifi_RxBuffer。 */
void Wifi_Poll(void);

/* 查询当前状态；WIFI_STATE_READY 时才能发送。 */
WifiState Wifi_GetState(void);

/* 返回状态名字符串：STARTING/READY/RETRY/ERROR，便于打印。 */
const char *Wifi_GetStateName(void);

/* ============================================================================
 * 发送
 * ==========================================================================*/

/* 发送一条自定义消息，自动拼成：
 *     <WIFI_DEVICE_ID>,<type>[,<data>]\r\n
 * 例如：Wifi_Send("LED", "1")       -> "STM32,LED,1\r\n"
 *       Wifi_Send("STATUS", NULL)   -> "STM32,STATUS\r\n"
 * 参数：
 *   type：消息类型，1~16 个字符，不能含逗号/回车/换行；
 *   data：数据，可为 NULL 或空串；可含逗号，但不能含回车/换行。
 * 返回：1=已交给 AT 驱动排队；0=未就绪/上一包未发完/参数非法。
 * 注意：返回 1 只表示“已排队”，真正发完看 Wifi_GetStats()->tx_text_ok
 *       和 WifiAt 的 tx_ok；不要把返回值当成对端已收到。 */
uint8_t Wifi_Send(const char *type, const char *data);

/* 发送一整行原始内容，函数自动补 \r\n。
 * 例如：Wifi_SendRaw("PC,PING,1") -> 发送 "PC,PING,1\r\n"。
 * line 不要包含 \r 或 \n；长度不能超过 WIFI_TX_MAX_SIZE-2。
 * 返回 1=已排队，0=未就绪/忙/参数非法。 */
uint8_t Wifi_SendRaw(const char *line);

/* 立即发送一帧球坐标（不经过 WIFI_BALL_PERIOD_MS 限速），seq 自动加 1。
 * 一般不需要手动调用；摄像头每来一个合法帧时用 Wifi_OnBallFrame() 即可。
 * 返回 1=已排队，0=链路忙/未就绪（同时累计 tx_ball_dropped）。 */
uint8_t Wifi_SendBall(uint8_t x, uint8_t y);

/* 发送一段文本；对端会去掉源名/类型前缀，串口1只打印 text 本身。
 * 例如：Wifi_SendText("hello world")
 * 线上仍是 "STM32B,TEXT,hello world\r\n"，但接收端只显示 "hello world"。
 * 返回 1=已排队，0=未就绪/忙/文本含换行或过长。 */
uint8_t Wifi_SendText(const char *text);

/* 直接发送一段字符串，接收端只打印这段内容。
 * 等价于 Wifi_SendText(text)，是日常最方便的一个参数发送接口。
 * 例如：Wifi_SendString("hello world") -> 对端串口1打印 "hello world"。 */
uint8_t Wifi_SendString(const char *text);

/* ============================================================================
 * 球坐标遥测与状态
 * ==========================================================================*/

/* 摄像头每收到一个合法帧调用一次。
 * 内部按 WIFI_BALL_PERIOD_MS 限速（默认 100ms 最多一帧），
 * 并自动累加 seq；发送忙时丢这一帧并累计 tx_ball_dropped，不阻塞 PID。 */
void Wifi_OnBallFrame(uint8_t x, uint8_t y);

/* 打印一行状态到串口1，串口命令 `wifi` 调用：
 * 状态、模式、ID、广播地址、AT 原始收发计数、应用层收发计数、远端球坐标。 */
void Wifi_PrintStatus(void);

/* 返回只读统计指针；不要修改其内容。 */
const WifiStats *Wifi_GetStats(void);

#endif

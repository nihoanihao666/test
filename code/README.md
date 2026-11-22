# 32test2222 WiFi 通信说明

这个工程是“第二块 STM32”，通过 USART2 接 ESP8266，加入第一块板子（test111）开的
`BallPlate` 热点，用 UDP 广播互相通信。

## 1. 接线

| STM32F103C8T6 | ESP8266 |
| --- | --- |
| PA9 / USART1_TX | USB-TTL 的 RX（电脑调试） |
| PA10 / USART1_RX | USB-TTL 的 TX |
| PA2 / USART2_TX | ESP8266 的 RX |
| PA3 / USART2_RX | ESP8266 的 TX |
| GND | GND |
| 3.3V | VCC / EN |

USART1、USART2 都是 **115200、8N1**。

## 2. 角色

```text
第一块板子 test111：ESP8266 开 AP "BallPlate"，192.168.4.1，UDP 8080
第二块板子 32test2222：ESP8266 作为 STA 加入 BallPlate，UDP 8080
```

第二块板子的设备名是 `STM32B`，第一块是 `STM32`，不能重复。

## 3. 文件

```text
code/wifi_config.h   热点名、密码、广播地址、端口、设备名、缓冲区
code/wifi_port.c     USART2 DMA1_Channel6 循环接收 + IDLE 中断；USART1 调试打印
code/wifi_at.c       ESP8266 AT 状态机（+++/AT/RST/STA/UDP/+IPD/CIPSEND）
code/wifi_app.c      对外 API：Wifi_Init/Poll/Send/接收缓冲
code/app_demo.c      上电自动发字符串的演示
```

`main.c` 只加了两个 USER CODE 调用：

```c
App_Init();   /* MX_USART1/2 初始化之后 */
...
App_Loop();   /* while(1) 里 */
```

## 4. 发送：一个函数传参

日常直接发字符串，用 `Wifi_SendString()`：

```c
#include "wifi_app.h"

Wifi_SendString("hello world");
/* 线上：STM32B,TEXT,hello world\r\n
 * 对端串口1只打印：hello world */

Wifi_SendText("hello");       /* 同上，Wifi_SendString 的别名 */
Wifi_Send("LED", "1");        /* 自定义类型：STM32B,LED,1\r\n */
Wifi_SendRaw("PC,PING,1");    /* 完全原样发一整行：PC,PING,1\r\n */
```

返回 `1` 只表示已交给 AT 驱动排队；真正发完看 `Wifi_GetStats()->tx_text_ok`
或串口1的 `wifi` 状态。

`Wifi_SendString` / `Wifi_SendText` 的正文可以带空格和逗号，不能带 `\r`、`\n`。
接收端对 `TEXT` 类型只打印正文，不打印 `STM32B,TEXT,` 前缀。

## 5. 接收：判断数组缓冲

```c
Wifi_Poll();

if (Wifi_RxFlag)
{
    /* Wifi_RxBuffer 是一整行，不含 \r\n，例如：
       "STM32,BALL,80,60,1" 或 "STM32,PONG,1" */
    if (strstr(Wifi_RxBuffer, "PONG") != NULL)
    {
        /* 收到第一块板子的回复 */
    }
    Wifi_ClearRxFlag();
}
```

`Wifi_RxBuffer` 只保留最新一条；处理慢时旧消息会被覆盖。

## 6. DMA 接收与 CubeMX

`32test2222.ioc` 里已经配置好：

- USART2_RX → **DMA1_Channel6**，循环模式，优先级 High；
- USART2 全局中断、DMA1_Channel6 中断，抢占优先级 1；
- `MX_DMA_Init` 排在 USART1/USART2 初始化之前。

CubeMX 会生成：

```text
Core/Src/dma.c              MX_DMA_Init()，开 DMA1 时钟和 DMA1_Channel6 中断
Core/Src/usart.c            hdma_usart2_rx、HAL_DMA_Init、__HAL_LINKDMA、USART2 NVIC
Core/Src/stm32f1xx_it.c     USART2_IRQHandler、DMA1_Channel6_IRQHandler
```

`code/wifi_port.c` 只负责：

- 调用 `HAL_UART_Receive_DMA()` 启动循环接收；
- 开 USART2 IDLE 中断；
- 在 IDLE / DMA 半满 / DMA 全满时，把新字节搬进软件环形队列；
- 提供 `WifiPort_Read()` 给 AT 驱动。

**`wifi_port.c` 里不再定义 `USART2_IRQHandler` / `DMA1_Channel6_IRQHandler`**，避免和
CubeMX 生成的重复。`USART2_IRQHandler` 里的 USER CODE 已经调用：

```c
WifiPort_UartIrqHandler(&huart2);
```

CubeMX 重新生成后：

1. `.ioc` 里的 DMA/NVIC 配置仍然在；
2. `USART2_IRQHandler` 的 USER CODE 调用会保留；
3. `code/` 文件不会被覆盖；
4. 检查 Keil 的 `code` group 和 `../code` include path 是否还在，若被删手动加回。

重新生成后检查：

1. `main.c` 的 USER CODE 里 `App_Init()` / `App_Loop()` 还在；
2. Keil 的 `code` group 和 `../code` include path 还在；
3. 不要额外在 CubeMX 里开 USART2 DMA/NVIC，否则会和 `code/wifi_port.c` 重复。

## 7. 上电测试

1. 先给第一块板子 test111 上电，等串口1出现：

```text
WiFi: AP "BallPlate" ready, UDP 192.168.4.255:8080
```

2. 再给第二块板子 32test2222 上电，串口1应出现：

```text
[BOARD2] 32test2222 WiFi demo
WiFi: ESP8266 starting...
WiFi: STA connected, UDP 192.168.4.255:8080
[BOARD2] ready; auto SEND every 1s
```

3. 第二块板子每秒自动发：

```text
Wifi_SendString("hello world")
```

线上是：

```text
STM32B,TEXT,hello world
```

第一块板子串口1只打印正文：

```text
hello world
```

`TEXT` 不会自动回复，所以第二块 `at_rx=0` 是正常的。

4. 想反向验证，在第一块板子的串口1发：

```text
wifi send TEXT hello_from_board1
```

第二块板子串口1只打印：

```text
hello_from_board1
```

5. 第一块板子接摄像头时，第二块板子还会打印：

```text
[WiFi RX] BALL STM32 x=80 y=60 seq=1
```

如果只是测试“发字符串、对端只打印内容”，看到第一块打印 `hello world` 就已经通了。
需要自动回包的测试才用 `PING`，日常发内容不需要。

## 8. 不想自动发送

把 `wifi_config.h` 的：

```c
#define WIFI_AUTO_SEND_ENABLE 0U
```

改成 0，重新编译。之后只在你的代码里调 `Wifi_SendString()` 发送。

## 9. 编译

Keil 打开：

```text
E:\bishe1111111111111111111\通信\wiff\32test2222\MDK-ARM\32test2222.uvprojx
```

Rebuild + Download。生成的 hex：

```text
E:\bishe1111111111111111111\通信\wiff\32test2222\MDK-ARM\32test2222\32test2222.hex
```

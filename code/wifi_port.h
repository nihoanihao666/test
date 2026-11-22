#ifndef WIFI_PORT_H
#define WIFI_PORT_H

#include <stdint.h>
#include "main.h"

/* ============================================================================
 * WiFi 移植层（本工程使用 USART2 + DMA1_Channel6 + IDLE 中断）
 *
 * 接收：DMA 循环写入 s_dma_buf；USART2 IDLE / DMA 半满 / DMA 全满时，
 *       把新增字节搬进软件环形队列 s_ring。
 * 发送：AT 指令和业务数据都很短，直接 HAL_UART_Transmit 阻塞发送。
 * 调试：USART1 阻塞打印。
 *
 * 换工程时改 wifi_config.h 里的 WIFI_UART/WIFI_DEBUG_UART，
 * 并在 CubeMX 里给对应的 UART_RX 配好 DMA 通道（本工程是 DMA1_Channel6）。
 * ==========================================================================*/

/* 在 MX_USART2_UART_Init() 之后调用：启动循环 DMA 接收并打开 IDLE 中断。
 * DMA 时钟、DMA 通道和 NVIC 由 CubeMX 生成的 dma.c/usart.c 配置。 */
void WifiPort_Init(void);

/* 非阻塞读 1 字节：1=读到，0=暂时没有。AT 驱动会反复调用。 */
int WifiPort_Read(uint8_t *byte, uint32_t *received_ms);

/* 阻塞发送一段数据：1=成功，0=失败。 */
uint8_t WifiPort_Write(const uint8_t *data, uint16_t length);

/* 毫秒时钟，给 AT 超时用。 */
uint32_t WifiPort_Now(void);

/* 调试打印到 USART1。 */
void WifiPort_Log(const char *text);

/* 在 USART2_IRQHandler 里调用：检测 IDLE 并把 DMA 新增数据搬进环形队列。 */
void WifiPort_UartIrqHandler(UART_HandleTypeDef *huart);

#endif

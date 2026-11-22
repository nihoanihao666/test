#include "wifi_port.h"
#include "wifi_config.h"
#include "usart.h"
#include <string.h>

/* USART2_RX 使用 DMA1_Channel6（STM32F103 固定映射）。
 * DMA 时钟、DMA1_Channel6 中断、USART2 中断由 CubeMX 生成的 dma.c/usart.c
 * 配置；本文件只负责启动 DMA、处理 IDLE 和把数据搬进软件环形队列。
 * 这样 CubeMX 重新生成不会和本文件重复定义中断函数。 */
static uint8_t s_dma_buf[WIFI_DMA_RX_SIZE];

/* DMA 搬运出来的字节先放这里，AT 驱动从 WifiPort_Read() 取。 */
static uint8_t s_ring[WIFI_RX_RING_SIZE];
static volatile uint16_t s_head;
static volatile uint16_t s_tail;

static uint16_t s_old_pos;   /* DMA 缓冲区上一次已经搬走到哪个位置 */
static uint8_t s_ready;

/* 把一个字节块写进软件环形队列；满了就丢弃新字节。
 * 只在中断里调用（USART2/DMA 中断优先级相同，不会互相打断）。 */
static void RingPush(const uint8_t *data, uint16_t length)
{
    uint16_t i;
    for (i = 0U; i < length; ++i)
    {
        uint16_t next = (uint16_t)((s_head + 1U) & (WIFI_RX_RING_SIZE - 1U));
        if (next == s_tail)
        {
            return; /* 队列满：丢新数据，避免覆盖未读数据 */
        }
        s_ring[s_head] = data[i];
        s_head = next;
    }
}

/* 计算 DMA 当前写位置，把自上次以来新增的字节搬进 s_ring。
 * 可能跨越缓冲区尾部，所以要分两段复制。
 * 在 IDLE、DMA 半满、DMA 全满三种事件里都会调用。 */
static void CopyDmaDelta(void)
{
    uint16_t pos = (uint16_t)(WIFI_DMA_RX_SIZE - __HAL_DMA_GET_COUNTER(WIFI_UART->hdmarx));

    /* 刚好写满 256 字节、计数器还没重装时，pos 可能等于 256。
     * 把它按“已经回绕到 0”处理，避免漏掉最后一段。 */
    if (pos >= WIFI_DMA_RX_SIZE)
    {
        if (s_old_pos < WIFI_DMA_RX_SIZE)
        {
            RingPush(&s_dma_buf[s_old_pos], (uint16_t)(WIFI_DMA_RX_SIZE - s_old_pos));
        }
        s_old_pos = 0U;
        return;
    }
    if (pos == s_old_pos)
    {
        return;
    }
    if (pos > s_old_pos)
    {
        RingPush(&s_dma_buf[s_old_pos], (uint16_t)(pos - s_old_pos));
    }
    else
    {
        RingPush(&s_dma_buf[s_old_pos], (uint16_t)(WIFI_DMA_RX_SIZE - s_old_pos));
        RingPush(&s_dma_buf[0], pos);
    }
    s_old_pos = pos;
}

void WifiPort_Init(void)
{
    if (s_ready)
    {
        return;
    }

    s_head = 0U;
    s_tail = 0U;
    s_old_pos = 0U;

    /* DMA1 时钟、DMA1_Channel6 中断、USART2 中断已经在 MX_DMA_Init() 和
     * MX_USART2_UART_Init() 的 HAL_UART_MspInit() 里配置好，
     * huart2.hdmarx 也已由 __HAL_LINKDMA 绑定。这里只启动循环接收。 */
    __HAL_UART_CLEAR_IDLEFLAG(WIFI_UART);
    if (HAL_UART_Receive_DMA(WIFI_UART, s_dma_buf, WIFI_DMA_RX_SIZE) != HAL_OK)
    {
        Error_Handler();
    }
    __HAL_UART_ENABLE_IT(WIFI_UART, UART_IT_IDLE);

    s_ready = 1U;
}

int WifiPort_Read(uint8_t *byte, uint32_t *received_ms)
{
    uint32_t mask;
    int result = 0;

    if ((byte == NULL) || !s_ready)
    {
        return 0;
    }

    mask = __get_PRIMASK();
    __disable_irq();
    if (s_tail != s_head)
    {
        *byte = s_ring[s_tail];
        s_tail = (uint16_t)((s_tail + 1U) & (WIFI_RX_RING_SIZE - 1U));
        result = 1;
    }
    __set_PRIMASK(mask);

    if (result && (received_ms != NULL))
    {
        *received_ms = HAL_GetTick();
    }
    return result;
}

uint8_t WifiPort_Write(const uint8_t *data, uint16_t length)
{
    if ((data == NULL) || (length == 0U))
    {
        return 0U;
    }
    /* 数据很短（AT 指令/一行协议），阻塞发送 100ms 足够。 */
    return (HAL_UART_Transmit(WIFI_UART, (uint8_t *)data, length, 100U) == HAL_OK) ? 1U : 0U;
}

uint32_t WifiPort_Now(void)
{
    return HAL_GetTick();
}

void WifiPort_Log(const char *text)
{
#if WIFI_DEBUG_LOG
    if (text != NULL)
    {
        (void)HAL_UART_Transmit(WIFI_DEBUG_UART, (uint8_t *)text,
                                (uint16_t)strlen(text), 100U);
    }
#else
    (void)text;
#endif
}

void WifiPort_UartIrqHandler(UART_HandleTypeDef *huart)
{
    if (huart != WIFI_UART)
    {
        return;
    }
    /* IDLE：一帧数据结束，把 DMA 里新增的字节搬走。 */
    if ((__HAL_UART_GET_FLAG(huart, UART_FLAG_IDLE) != RESET) &&
        (__HAL_UART_GET_IT_SOURCE(huart, UART_IT_IDLE) != RESET))
    {
        __HAL_UART_CLEAR_IDLEFLAG(huart);
        CopyDmaDelta();
    }
}

/* DMA 半满：先搬前半段，避免数据在缓冲区回绕时丢失。 */
void HAL_UART_RxHalfCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart == WIFI_UART)
    {
        CopyDmaDelta();
    }
}

/* DMA 全满：搬后半段/回绕段。 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart == WIFI_UART)
    {
        CopyDmaDelta();
    }
}

/* 串口错误：停掉 DMA，清 IDLE，重新启动循环接收。 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart != WIFI_UART)
    {
        return;
    }
    (void)HAL_UART_AbortReceive(huart);
    s_old_pos = 0U;
    __HAL_UART_CLEAR_IDLEFLAG(huart);
    if (HAL_UART_Receive_DMA(huart, s_dma_buf, WIFI_DMA_RX_SIZE) != HAL_OK)
    {
        Error_Handler();
    }
    __HAL_UART_ENABLE_IT(huart, UART_IT_IDLE);
}

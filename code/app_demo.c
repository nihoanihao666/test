#include "app_demo.h"
#include "wifi_app.h"
#include "wifi_config.h"
#include "wifi_port.h"

static uint32_t s_last_send_ms;
static uint32_t s_last_status_ms;
static uint8_t s_ready_reported;

void App_Init(void)
{
    WifiPort_Log("\r\n[BOARD2] 32test2222 WiFi demo\r\n");
    WifiPort_Log("[BOARD2] USART2=ESP8266, USART1=debug, 115200\r\n");

    /* Wifi_Init 内部会初始化 AT 状态机，并调用 WifiPort_Init 启动 DMA 接收。
     * 必须在 MX_USART1_UART_Init/MX_USART2_UART_Init 之后调用。 */
    Wifi_Init();
}

void App_Loop(void)
{
    uint32_t now = HAL_GetTick();

    /* 推进 AT 配置、发送、+IPD 解析；不阻塞。 */
    Wifi_Poll();

    if ((Wifi_GetState() == WIFI_STATE_READY) && !s_ready_reported)
    {
        s_ready_reported = 1U;
        s_last_status_ms = now;
        Wifi_PrintStatus();
#if WIFI_AUTO_SEND_ENABLE
        WifiPort_Log("[BOARD2] ready; auto SEND every 3s\r\n");
#else
        WifiPort_Log("[BOARD2] ready; auto SEND disabled\r\n");
#endif
    }
    else if (Wifi_GetState() != WIFI_STATE_READY)
    {
        s_ready_reported = 0U;
    }

    /* 每 5 秒打印一次 at_tx/at_rx/app_rx，方便确认到底有没有发/收。 */
    if ((Wifi_GetState() == WIFI_STATE_READY) &&
        ((now - s_last_status_ms) >= 5000U))
    {
        s_last_status_ms = now;
        Wifi_PrintStatus();
    }

    /* 收到一条完整消息时，驱动已经打印了内容本身（TEXT 只打印正文，
     * 不带 "STM32B,TEXT," 前缀）。这里只清标志；如果要自己处理，
     * 判断 Wifi_RxFlag 并读 Wifi_RxBuffer 即可。 */
    if (Wifi_RxFlag)
    {
        Wifi_ClearRxFlag();
    }

#if WIFI_AUTO_SEND_ENABLE
    /* 测试用：每秒直接发一段字符串。
     * 不需要自动发就把 wifi_config.h 的 WIFI_AUTO_SEND_ENABLE 改成 0。 */
    if ((Wifi_GetState() == WIFI_STATE_READY) &&
        ((now - s_last_send_ms) >= WIFI_AUTO_SEND_MS))
    {
			uint8_t sent = Wifi_SendString("你好昊天");
        s_last_send_ms = now;
        WifiPort_Log(sent ? "[BOARD2] TX 你好小智\r\n" : "[BOARD2] TX FAILED\r\n");
    }
#endif
}

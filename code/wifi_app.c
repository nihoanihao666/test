#include "wifi_app.h"
#include "wifi_at.h"
#include "wifi_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if WIFI_NET_MODE == WIFI_NET_MODE_AP
#define WIFI_MODE_TEXT "AP"
#else
#define WIFI_MODE_TEXT "STA"
#endif

static WifiStats s_stats;
volatile uint8_t Wifi_RxFlag;
char Wifi_RxBuffer[WIFI_RX_BUFFER_SIZE];
static uint32_t s_last_ball_ms;
static uint32_t s_last_ball_log_ms;
static uint16_t s_ball_seq;
static uint8_t s_ball_started;
static uint8_t s_ready_reported;
static uint8_t s_hello_pending;

static char s_rx_line[WIFI_APP_LINE_SIZE];
static uint16_t s_rx_len;
static uint8_t s_rx_overflow;

static uint8_t SendLine(const char *line)
{
    size_t length = strlen(line);
    if ((length == 0U) || (length > WIFI_TX_MAX_SIZE))
    {
        return 0U;
    }
    return WifiAt_Send((const uint8_t *)line, (uint16_t)length);
}

static void LogRx(const char *line)
{
#if WIFI_LOG_RX
    /* 只打印收到的内容本身，不加 [WiFi RX] 前缀。 */
    WifiPort_Log(line);
    WifiPort_Log("\r\n");
#else
    (void)line;
#endif
}

static uint8_t ParseU8(const char *text, uint8_t *value)
{
    char *end;
    unsigned long n;
    if ((text == NULL) || (*text == '\0'))
    {
        return 0U;
    }
    n = strtoul(text, &end, 10);
    if ((*end != '\0') || (n > 255U))
    {
        return 0U;
    }
    *value = (uint8_t)n;
    return 1U;
}

static uint8_t ParseU16(const char *text, uint16_t *value)
{
    char *end;
    unsigned long n;
    if ((text == NULL) || (*text == '\0'))
    {
        return 0U;
    }
    n = strtoul(text, &end, 10);
    if ((*end != '\0') || (n > 65535U))
    {
        return 0U;
    }
    *value = (uint16_t)n;
    return 1U;
}

/* 处理一行业务协议；只做遥测/回显，不直接改 PID 或舵机。 */
static void HandleLine(char *line)
{
    char original[WIFI_APP_LINE_SIZE];
    char *fields[6];
    unsigned int count = 0U;
    char *p = line;

    if (*line == '\0')
    {
        return;
    }
    strncpy(original, line, sizeof(original) - 1U);
    original[sizeof(original) - 1U] = '\0';

    while ((count < 6U) && (p != NULL))
    {
        fields[count++] = p;
        p = strchr(p, ',');
        if (p != NULL)
        {
            *p++ = '\0';
        }
    }

    /* 自己发的广播可能被 AP 回环回来，按源名字丢掉。 */
    if (strcmp(fields[0], WIFI_DEVICE_ID) == 0)
    {
        return;
    }

    ++s_stats.rx_lines;

    /* 给应用层留一份“最新一条完整消息”：判断 Wifi_RxFlag 即可，
     * 内容不含 \r\n。只保留最新一条，处理慢时旧消息会被覆盖。 */
    strncpy(Wifi_RxBuffer, original, sizeof(Wifi_RxBuffer) - 1U);
    Wifi_RxBuffer[sizeof(Wifi_RxBuffer) - 1U] = '\0';
    Wifi_RxFlag = 1U;

    /* HELLO 只是上线握手，不打印，避免串口1出现协议前缀。 */
    if ((count >= 2U) && (strcmp(fields[1], "HELLO") == 0))
    {
        return;
    }

    /* ZONE 是给小智/板球端用的区域目标，第二块只做旁听，不打印协议行。 */
    if ((count >= 3U) && (strcmp(fields[1], "ZONE") == 0))
    {
        return;
    }

    if ((count >= 2U) && (strcmp(fields[1], "TEXT") == 0))
    {
        /* TEXT 只打印正文，不打印 "STM32B,TEXT," 前缀。
         * 从 original 里找第二个逗号，后面全部原样输出，允许正文带空格/逗号。 */
#if WIFI_LOG_RX
        const char *payload = strchr(original, ',');
        if (payload != NULL)
        {
            payload = strchr(payload + 1U, ',');
        }
        if (payload != NULL)
        {
            WifiPort_Log(payload + 1U);
            WifiPort_Log("\r\n");
        }
#endif
        return;
    }

    if ((count >= 2U) && (strcmp(fields[1], "PING") == 0))
    {
        char reply[48];
        const char *token = (count >= 3U) ? fields[2] : "0";
        snprintf(reply, sizeof(reply), "%s,PONG,%s\r\n", WIFI_DEVICE_ID, token);
        if (SendLine(reply))
        {
            ++s_stats.tx_text_ok;
        }
        LogRx(original);
        return;
    }

    if ((count >= 4U) && (strcmp(fields[1], "BALL") == 0))
    {
        uint8_t x, y;
        uint16_t seq = 0U;
        if (ParseU8(fields[2], &x) && ParseU8(fields[3], &y) &&
            ((count < 5U) || ParseU16(fields[4], &seq)))
        {
            uint32_t now = WifiPort_Now();
            s_stats.remote_ball_valid = 1U;
            s_stats.remote_ball_x = x;
            s_stats.remote_ball_y = y;
            s_stats.remote_ball_seq = seq;
            /* 球坐标是周期数据，最多每 500ms 打印一次，避免串口1被刷屏。 */
            if ((now - s_last_ball_log_ms) >= 500U)
            {
                char text[96];
                s_last_ball_log_ms = now;
                snprintf(text, sizeof(text), "[WiFi RX] BALL %s x=%u y=%u seq=%u\r\n",
                         fields[0], (unsigned)x, (unsigned)y, (unsigned)seq);
                WifiPort_Log(text);
            }
            return;
        }
        ++s_stats.rx_bad;
        LogRx(original);
        return;
    }

    /* PING/BALL 之外的任何内容都原样打印，方便用串口助手测试。 */
    LogRx(original);
}

static void Feed(const uint8_t *data, uint16_t length)
{
    uint16_t i;
    for (i = 0U; i < length; ++i)
    {
        uint8_t byte = data[i];
        if (byte == '\n')
        {
            if (!s_rx_overflow && (s_rx_len != 0U))
            {
                s_rx_line[s_rx_len] = '\0';
                HandleLine(s_rx_line);
            }
            else if (s_rx_overflow)
            {
                ++s_stats.rx_bad;
            }
            s_rx_len = 0U;
            s_rx_overflow = 0U;
        }
        else if (byte != '\r')
        {
            if (s_rx_len < (sizeof(s_rx_line) - 1U))
            {
                s_rx_line[s_rx_len++] = (char)byte;
            }
            else
            {
                s_rx_overflow = 1U;
            }
        }
    }
}

void Wifi_Init(void)
{
    memset(&s_stats, 0, sizeof(s_stats));
    s_last_ball_ms = 0U;
    s_last_ball_log_ms = 0U;
    s_ball_seq = 0U;
    s_ball_started = 0U;
    s_ready_reported = 0U;
    s_hello_pending = 0U;
    s_rx_len = 0U;
    s_rx_overflow = 0U;
    Wifi_RxFlag = 0U;
    Wifi_RxBuffer[0] = '\0';
    WifiAt_Init();
}

void Wifi_ClearRxFlag(void)
{
    Wifi_RxFlag = 0U;
}

void Wifi_Poll(void)
{
    uint8_t buffer[64];
    uint16_t length;
    unsigned int i;
    WifiState state;

    WifiAt_Poll();
    state = Wifi_GetState();

    if ((state == WIFI_STATE_READY) && !s_ready_reported)
    {
        s_ready_reported = 1U;
        s_hello_pending = 1U;
        WifiPort_Log("WiFi: link ready, protocol " WIFI_DEVICE_ID ",HELLO/TEXT/PING/BALL\r\n");
    }
    else if (state != WIFI_STATE_READY)
    {
        s_ready_reported = 0U;
        s_hello_pending = 0U;
    }

    if (s_hello_pending)
    {
        char hello[32];
        snprintf(hello, sizeof(hello), "%s,HELLO,1\r\n", WIFI_DEVICE_ID);
        if (SendLine(hello))
        {
            s_hello_pending = 0U;
            ++s_stats.tx_text_ok;
        }
    }

    /* 每次最多取 4 批，避免持续输入时主循环一直停在解析上。 */
    for (i = 0U; i < 4U; ++i)
    {
        length = WifiAt_Read(buffer, sizeof(buffer));
        if (length == 0U)
        {
            break;
        }
        Feed(buffer, length);
    }
}

static uint8_t SendBallNow(uint8_t x, uint8_t y)
{
    char line[48];
    ++s_ball_seq;
    snprintf(line, sizeof(line), "%s,BALL,%u,%u,%u\r\n",
             WIFI_DEVICE_ID, (unsigned)x, (unsigned)y, (unsigned)s_ball_seq);
    if (SendLine(line))
    {
        ++s_stats.tx_ball_ok;
        return 1U;
    }
    /* 球坐标是周期数据：发送忙时直接丢这一帧，不排队补发旧坐标。 */
    ++s_stats.tx_ball_dropped;
    return 0U;
}

uint8_t Wifi_SendBall(uint8_t x, uint8_t y)
{
    return SendBallNow(x, y);
}

void Wifi_OnBallFrame(uint8_t x, uint8_t y)
{
#if WIFI_BALL_TELEMETRY_ENABLE
    uint32_t now = WifiPort_Now();

    if (s_ball_started && ((now - s_last_ball_ms) < WIFI_BALL_PERIOD_MS))
    {
        return;
    }
    s_ball_started = 1U;
    s_last_ball_ms = now;
    (void)SendBallNow(x, y);
#else
    (void)x;
    (void)y;
#endif
}

uint8_t Wifi_Send(const char *type, const char *data)
{
    char line[WIFI_TX_MAX_SIZE];
    size_t id_len, type_len, data_len = 0U;

    if (type == NULL)
    {
        return 0U;
    }
    type_len = strlen(type);
    id_len = strlen(WIFI_DEVICE_ID);
    if ((type_len == 0U) || (type_len > 16U) ||
        (strchr(type, ',') != NULL) || (strchr(type, '\r') != NULL) ||
        (strchr(type, '\n') != NULL))
    {
        return 0U;
    }
    if (data != NULL)
    {
        data_len = strlen(data);
        if ((strchr(data, '\r') != NULL) || (strchr(data, '\n') != NULL))
        {
            return 0U;
        }
    }
    if ((id_len + 1U + type_len + ((data_len != 0U) ? (1U + data_len) : 0U) + 2U) >= sizeof(line))
    {
        return 0U;
    }

    if (data_len != 0U)
    {
        snprintf(line, sizeof(line), "%s,%s,%s\r\n", WIFI_DEVICE_ID, type, data);
    }
    else
    {
        snprintf(line, sizeof(line), "%s,%s\r\n", WIFI_DEVICE_ID, type);
    }
    if (!SendLine(line))
    {
        return 0U;
    }
    ++s_stats.tx_text_ok;
    return 1U;
}

uint8_t Wifi_SendRaw(const char *line)
{
    char buffer[WIFI_TX_MAX_SIZE];
    size_t length;

    if (line == NULL)
    {
        return 0U;
    }
    length = strlen(line);
    if ((length == 0U) || (length > (sizeof(buffer) - 3U)) ||
        (strchr(line, '\r') != NULL) || (strchr(line, '\n') != NULL))
    {
        return 0U;
    }
    memcpy(buffer, line, length);
    buffer[length++] = '\r';
    buffer[length++] = '\n';
    buffer[length] = '\0';
    if (!SendLine(buffer))
    {
        return 0U;
    }
    ++s_stats.tx_text_ok;
    return 1U;
}

uint8_t Wifi_SendText(const char *text)
{
    return Wifi_Send("TEXT", text);
}

uint8_t Wifi_SendString(const char *text)
{
    /* 直接发一段字符串。接收端收到 TEXT 后只打印正文，不打印
     * "STM32B,TEXT," 前缀，所以串口1看到的就是 text 本身。 */
    return Wifi_SendText(text);
}

WifiState Wifi_GetState(void)
{
    switch (WifiAt_GetState())
    {
        case WIFI_AT_STARTING: return WIFI_STATE_STARTING;
        case WIFI_AT_READY: return WIFI_STATE_READY;
        case WIFI_AT_ERROR: return WIFI_STATE_ERROR;
        case WIFI_AT_OFF:
        default: return WIFI_STATE_OFF;
    }
}

const char *Wifi_GetStateName(void)
{
    switch (Wifi_GetState())
    {
        case WIFI_STATE_STARTING: return "STARTING";
        case WIFI_STATE_READY: return "READY";
        case WIFI_STATE_ERROR: return "ERROR";
        case WIFI_STATE_OFF:
        default: return "RETRY";
    }
}

void Wifi_PrintStatus(void)
{
    const WifiAtStats *at = WifiAt_GetStats();
    char text[240];

    snprintf(text, sizeof(text),
             "wifi=%s mode=%s id=%s udp=%s:%u at_uart=%lu at_tx=%lu at_fail=%lu at_rx=%lu "
             "at_drop=%lu retry=%lu app_rx=%lu bad=%lu ball=%lu/%lu remote=%u,%u seq=%u\r\n",
             Wifi_GetStateName(), WIFI_MODE_TEXT, WIFI_DEVICE_ID,
             WifiAt_GetBroadcastIp(), (unsigned)WIFI_UDP_PORT,
             (unsigned long)at->uart_rx_bytes,
             (unsigned long)at->tx_ok, (unsigned long)at->tx_failed,
             (unsigned long)at->rx_bytes, (unsigned long)at->rx_dropped,
             (unsigned long)at->retries,
             (unsigned long)s_stats.rx_lines, (unsigned long)s_stats.rx_bad,
             (unsigned long)s_stats.tx_ball_ok, (unsigned long)s_stats.tx_ball_dropped,
             (unsigned)s_stats.remote_ball_x, (unsigned)s_stats.remote_ball_y,
             (unsigned)s_stats.remote_ball_seq);
    WifiPort_Log(text);
}

const WifiStats *Wifi_GetStats(void)
{
    return &s_stats;
}

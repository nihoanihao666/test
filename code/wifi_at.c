#include "wifi_at.h"
#include "wifi_config.h"
#include "wifi_port.h"

#include <stdio.h>
#include <string.h>

/* AT 回复事件位：分别表示成功、失败、模块提示符、发送成功、模块启动完成、WiFi断开。 */
enum
{
    EV_OK = 1U,
    EV_ERROR = 2U,
    EV_PROMPT = 4U,
    EV_SEND_OK = 8U,
    EV_READY = 16U,
    EV_DISCONNECT = 32U
};

/* 配置步骤；正常配置时使用“当前步骤 + 1”进入下一步。 */
typedef enum
{
    STEP_GUARD,          /* 发送 +++ 前保持串口静默 */
    STEP_ESCAPE,         /* 已发 +++，继续静默以退出残留透传 */
    STEP_SYNC,           /* AT 握手，确认模块和波特率 */
    STEP_CLEAR_AUTO,     /* 关闭开机自动透传 */
    STEP_RESET,          /* 软复位，等待 ready */
    STEP_ECHO,           /* 关闭 AT 回显 */
    STEP_STORE,          /* 尽量不把临时配置写 Flash */
    STEP_MODE,           /* AP 或 STA 模式 */
    STEP_NORMAL,         /* 普通 AT 收发模式（非透传） */
    STEP_MUX,            /* 单连接 */
    STEP_DINFO,          /* +IPD 包头不带远端 IP/端口 */
    STEP_RECV_MODE,      /* 主动上报接收数据 */
    STEP_AP_IP,          /* AP 固定 IP，STA 跳过 */
    STEP_DHCP,           /* AP 打开 DHCP，STA 跳过 */
    STEP_NETWORK,        /* AP 建热点 / STA 加入热点 */
    STEP_SOCKET,         /* 建立 UDP 广播套接字 */
    STEP_RUN,            /* 正常运行 */
    STEP_RETRY,          /* 等待后重新配置 */
    STEP_FAULT           /* 收发状态不确定，等待人工复位 */
} Step;

typedef enum { TX_IDLE, TX_BEGIN, TX_PROMPT, TX_ACK } TxStep;

#if WIFI_NET_MODE == WIFI_NET_MODE_AP
#define WIFI_CWMODE_VALUE 2U
#else
#define WIFI_CWMODE_VALUE 1U
#endif

static WifiAtState s_state = WIFI_AT_OFF;
static WifiAtStats s_stats;
static const char *s_error = "none";
static Step s_step = STEP_GUARD;
static uint8_t s_initialized;
static uint8_t s_waiting;       /* 已发指令，等待回复 */
static uint8_t s_variant;       /* 指令兼容分支：0=_CUR，1=普通 */
static uint8_t s_events;        /* AT 回复事件位 */
static uint32_t s_started;      /* 当前步骤开始时刻 */
static uint32_t s_timeout;      /* 当前步骤超时时间 */

static char s_line[WIFI_AT_LINE_SIZE]; /* 尚未遇到换行的 AT 回复/包头 */
static uint16_t s_line_len;
static uint8_t s_line_overflow;

static uint32_t s_payload_left;         /* 当前 +IPD 数据区还剩多少字节 */
static uint8_t s_rx[WIFI_RX_RING_SIZE]; /* 已剥离 +IPD 包头的业务数据队列 */
static uint16_t s_rx_write, s_rx_read;

static uint8_t s_tx[WIFI_TX_MAX_SIZE];  /* 当前待发送的数据副本 */
static uint16_t s_tx_length;
static TxStep s_tx_step;
static uint32_t s_tx_started;
static uint8_t s_ready_logged;
static const char *s_active_broadcast = WIFI_BROADCAST_IP;

static void parser_reset(void)
{
    s_line_len = 0U;
    s_line_overflow = 0U;
    s_payload_left = 0U;
    s_events = 0U;
}

static void change_step(Step step)
{
    s_step = step;
    s_waiting = 0U;
    s_variant = 0U;
    s_ready_logged = 0U;
    s_started = WifiPort_Now();
}

/* ambiguous=1：模块可能仍在等待定长业务数据，继续发 AT 会污染数据，需断电复位。 */
static void fail(const char *reason, uint8_t ambiguous)
{
    s_error = reason;
    if (s_tx_step != TX_IDLE)
    {
        ++s_stats.tx_failed;
    }
    s_tx_step = TX_IDLE;
    s_tx_length = 0U;
    s_state = ambiguous ? WIFI_AT_ERROR : WIFI_AT_OFF;
    if (!ambiguous)
    {
        ++s_stats.retries;
    }
    change_step(ambiguous ? STEP_FAULT : STEP_RETRY);
    WifiPort_Log("WiFi: ");
    WifiPort_Log(reason);
    WifiPort_Log(ambiguous ? " (reset both boards)\r\n" : " (retry)\r\n");
}

/* 从 cursor 读取十进制数，最大 65535；成功返回 1 并推进指针。 */
static uint8_t number(const char **cursor, uint32_t *value)
{
    const char *p = *cursor;
    uint32_t n = 0U;
    if ((*p < '0') || (*p > '9'))
    {
        return 0U;
    }
    while ((*p >= '0') && (*p <= '9'))
    {
        if ((n > 6553U) || ((n == 6553U) && (*p > '5')))
        {
            return 0U;
        }
        n = n * 10U + (uint32_t)(*p++ - '0');
    }
    *cursor = p;
    *value = n;
    return 1U;
}

/* 解析 +IPD,<长度>: 或 +IPD,<连接ID>,<长度>:；成功切换为定长数据接收。 */
static uint8_t ipd_header(void)
{
    const char *p;
    uint32_t first, length;
    if (strncmp(s_line, "+IPD,", 5U) != 0)
    {
        return 0U;
    }
    p = s_line + 5;
    if (!number(&p, &first))
    {
        return 0U;
    }
    if (*p == ',')
    {
        ++p;
        if ((first > 4U) || !number(&p, &length))
        {
            return 0U;
        }
    }
    else
    {
        length = first;
    }
    if (*p != '\0')
    {
        return 0U;
    }
    s_payload_left = length;
    return 1U;
}

/* 处理完整 AT 文本行；业务数据不进入这里。 */
static void line_received(void)
{
    if (strcmp(s_line, "OK") == 0)
    {
        s_events |= EV_OK;
    }
    else if (strcmp(s_line, "SEND OK") == 0)
    {
        s_events |= EV_SEND_OK;
    }
    else if (strcmp(s_line, "ready") == 0)
    {
        s_events |= EV_READY;
    }
    else if (strcmp(s_line, "ALREADY CONNECT") == 0)
    {
        s_events |= EV_OK;
    }
    else if ((strcmp(s_line, "WIFI DISCONNECT") == 0) || (strcmp(s_line, "CLOSED") == 0))
    {
        s_events |= EV_DISCONNECT;
    }
    else if ((strcmp(s_line, "ERROR") == 0) || (strcmp(s_line, "FAIL") == 0) ||
             (strcmp(s_line, "SEND FAIL") == 0) || (strncmp(s_line, "busy", 4U) == 0) ||
             (strcmp(s_line, "link is not valid") == 0))
    {
        s_events |= EV_ERROR;
    }
}

static void push_payload(uint8_t byte)
{
    uint16_t next = (uint16_t)((s_rx_write + 1U) & (WIFI_RX_RING_SIZE - 1U));
    if (next == s_rx_read)
    {
        ++s_stats.rx_dropped;
        return;
    }
    s_rx[s_rx_write] = byte;
    s_rx_write = next;
}

/* 字节流解析入口；AT 回复、+IPD 包头和数据区允许跨多次接收到达。 */
static void parse_byte(uint8_t byte)
{
    if (s_payload_left != 0U)
    {
        push_payload(byte);
        --s_payload_left;
        return;
    }
    if (byte == '\r')
    {
        return;
    }
    if (byte == '\n')
    {
        if (!s_line_overflow && (s_line_len != 0U))
        {
            s_line[s_line_len] = '\0';
            line_received();
        }
        s_line_len = 0U;
        s_line_overflow = 0U;
        return;
    }
    if (s_line_overflow)
    {
        return;
    }
    if ((byte == '>') && (s_line_len == 0U))
    {
        s_events |= EV_PROMPT;
        return;
    }
    if ((byte == ' ') && (s_line_len == 0U))
    {
        return;
    }
    if (byte == ':')
    {
        s_line[s_line_len] = '\0';
        if (ipd_header())
        {
            s_line_len = 0U;
            return;
        }
    }
    if (s_line_len < sizeof(s_line) - 1U)
    {
        s_line[s_line_len++] = (char)byte;
    }
    else
    {
        s_line_overflow = 1U;
    }
}

/* 热点名/密码只接受可安全放进 AT 双引号参数的字符。 */
static uint8_t valid_text(const char *text, uint16_t maximum)
{
    uint16_t n;
    if (text == NULL)
    {
        return 0U;
    }
    for (n = 0U; text[n] != '\0'; ++n)
    {
        if ((n >= maximum) || (text[n] == '"') || (text[n] == '\\') ||
            (text[n] == ',') || (text[n] == '\r') || (text[n] == '\n'))
        {
            return 0U;
        }
    }
    return (uint8_t)(n != 0U);
}

/* 只接受四段 0~255 的 IPv4，不接受域名。 */
static uint8_t valid_ip(const char *text)
{
    uint8_t part;
    uint32_t value;
    const char *p = text;
    if (p == NULL)
    {
        return 0U;
    }
    for (part = 0U; part < 4U; ++part)
    {
        if (!number(&p, &value) || (value > 255U))
        {
            return 0U;
        }
        if (part == 3U)
        {
            return (uint8_t)(*p == '\0');
        }
        if (*p++ != '.')
        {
            return 0U;
        }
    }
    return 0U;
}

void WifiAt_Init(void)
{
    memset(&s_stats, 0, sizeof(s_stats));
    s_error = "none";
    s_rx_read = s_rx_write = 0U;
    s_tx_length = 0U;
    s_tx_step = TX_IDLE;
    s_active_broadcast = WIFI_BROADCAST_IP;
    parser_reset();

#if WIFI_NET_MODE == WIFI_NET_MODE_AP
    if (!valid_text(WIFI_AP_SSID, 32U) || !valid_text(WIFI_AP_PASS, 64U) ||
        (strlen(WIFI_AP_PASS) < 8U))
    {
        s_error = "bad AP SSID/password (password >= 8 chars)";
        s_state = WIFI_AT_ERROR;
        s_step = STEP_FAULT;
        s_initialized = 1U;
        WifiPort_Log("WiFi: bad AP SSID/password in wifi_config.h\r\n");
        return;
    }
#else
    if (!valid_text(WIFI_STA_SSID, 32U) || !valid_text(WIFI_STA_PASS, 64U))
    {
        s_error = "bad STA SSID/password";
        s_state = WIFI_AT_ERROR;
        s_step = STEP_FAULT;
        s_initialized = 1U;
        WifiPort_Log("WiFi: bad STA SSID/password in wifi_config.h\r\n");
        return;
    }
#endif
    if (!valid_ip(WIFI_BROADCAST_IP) || (WIFI_UDP_PORT == 0U))
    {
        s_error = "bad broadcast IP/UDP port";
        s_state = WIFI_AT_ERROR;
        s_step = STEP_FAULT;
        s_initialized = 1U;
        WifiPort_Log("WiFi: bad broadcast IP/UDP port in wifi_config.h\r\n");
        return;
    }

    WifiPort_Init();
    s_initialized = 1U;
    s_state = WIFI_AT_STARTING;
    change_step(STEP_GUARD);
    WifiPort_Log("WiFi: ESP8266 starting...\r\n");
}

/* 根据当前步骤构造一条 AT 指令；返回 0 表示当前模式跳过该步骤。 */
static uint8_t command_for_step(char *cmd, uint16_t capacity)
{
    const char *suffix = (s_variant == 0U) ? "_CUR" : "";
    s_timeout = 2000U;
    switch (s_step)
    {
        case STEP_SYNC:
            snprintf(cmd, capacity, (s_variant == 0U) ? "\r\nAT\r\n" : "AT\r\n");
            s_timeout = 1000U;
            break;
        case STEP_CLEAR_AUTO:
            snprintf(cmd, capacity, "AT+SAVETRANSLINK=0\r\n");
            break;
        case STEP_RESET:
            snprintf(cmd, capacity, "AT+RST\r\n");
            s_timeout = 6000U;
            break;
        case STEP_ECHO:
            snprintf(cmd, capacity, "ATE0\r\n");
            break;
        case STEP_STORE:
            snprintf(cmd, capacity, "AT+SYSSTORE=0\r\n");
            break;
        case STEP_MODE:
            snprintf(cmd, capacity, "AT+CWMODE%s=%u\r\n", suffix, (unsigned)WIFI_CWMODE_VALUE);
            break;
        case STEP_NORMAL:
            snprintf(cmd, capacity, "AT+CIPMODE=0\r\n");
            break;
        case STEP_MUX:
            snprintf(cmd, capacity, "AT+CIPMUX=0\r\n");
            break;
        case STEP_DINFO:
            snprintf(cmd, capacity, "AT+CIPDINFO=0\r\n");
            break;
        case STEP_RECV_MODE:
            snprintf(cmd, capacity, "AT+CIPRECVMODE=0\r\n");
            break;
        case STEP_AP_IP:
#if WIFI_NET_MODE == WIFI_NET_MODE_AP
            snprintf(cmd, capacity, "AT+CIPAP%s=\"%s\",\"%s\",\"%s\"\r\n",
                     suffix, WIFI_AP_IP, WIFI_AP_GATEWAY, WIFI_AP_NETMASK);
            break;
#else
            return 0U;
#endif
        case STEP_DHCP:
#if WIFI_NET_MODE == WIFI_NET_MODE_AP
            if (s_variant == 0U)
            {
                snprintf(cmd, capacity, "AT+CWDHCP_CUR=0,1\r\n");
            }
            else if (s_variant == 1U)
            {
                snprintf(cmd, capacity, "AT+CWDHCP=0,1\r\n");
            }
            else
            {
                snprintf(cmd, capacity, "AT+CWDHCP=2,1\r\n");
            }
            break;
#else
            return 0U;
#endif
        case STEP_NETWORK:
#if WIFI_NET_MODE == WIFI_NET_MODE_AP
            /* 最后一个 0 是 <ssid hidden>：强制广播 SSID。
             * 不写这个参数时，模块可能沿用之前保存的“隐藏热点”设置，
             * 电脑就会搜不到 BallPlate。 */
            snprintf(cmd, capacity, "AT+CWSAP%s=\"%s\",\"%s\",%u,3,4,0\r\n",
                     suffix, WIFI_AP_SSID, WIFI_AP_PASS, (unsigned)WIFI_AP_CHANNEL);
            s_timeout = 5000U;
#else
            snprintf(cmd, capacity, "AT+CWJAP%s=\"%s\",\"%s\"\r\n",
                     suffix, WIFI_STA_SSID, WIFI_STA_PASS);
            s_timeout = 25000U;
#endif
            break;
        case STEP_SOCKET:
            /* UDP mode=0：远端固定为广播地址，收到任何设备的数据都不改变发送目标。
             * 这样 CIPSEND 不需要再带远端 IP，兼容性最好。
             * 第一次用配置的广播地址；个别旧固件只认 255.255.255.255 时，
             * 下面的错误处理会自动用全局广播再试一次。 */
            snprintf(cmd, capacity, "AT+CIPSTART=\"UDP\",\"%s\",%u,%u,0\r\n",
                     (s_variant == 0U) ? WIFI_BROADCAST_IP : "255.255.255.255",
                     (unsigned)WIFI_UDP_PORT, (unsigned)WIFI_UDP_PORT);
            s_timeout = 8000U;
            break;
        default:
            return 0U;
    }
    return 1U;
}

static const char *step_name(void)
{
    switch (s_step)
    {
        case STEP_SYNC: return "AT handshake: check USART2 baud/wiring/power";
        case STEP_RESET: return "AT+RST / ready";
        case STEP_CLEAR_AUTO: return "SAVETRANSLINK";
        case STEP_ECHO: return "ATE0";
        case STEP_MODE: return "CWMODE";
        case STEP_NORMAL: return "CIPMODE";
        case STEP_MUX: return "CIPMUX";
        case STEP_DINFO: return "CIPDINFO";
        case STEP_RECV_MODE: return "CIPRECVMODE";
        case STEP_AP_IP: return "CIPAP";
        case STEP_DHCP: return "CWDHCP";
        case STEP_NETWORK: return "CWSAP / CWJAP: check SSID/password";
        case STEP_SOCKET: return "CIPSTART UDP: check broadcast IP/port";
        default: return "AT configuration";
    }
}

static void tx_next(uint8_t success)
{
    if (success)
    {
        ++s_stats.tx_ok;
    }
    else
    {
        ++s_stats.tx_failed;
        s_error = "UDP send rejected by module";
    }
    s_tx_step = TX_IDLE;
    s_tx_length = 0U;
}

/* 推进一批数据的发送；等待回复时立即返回。 */
static void send_poll(void)
{
    char cmd[96];
    if (s_tx_step == TX_BEGIN)
    {
        if ((s_state != WIFI_AT_READY) || (s_tx_length == 0U))
        {
            s_tx_step = TX_IDLE;
            return;
        }
        snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%u\r\n", (unsigned)s_tx_length);
        s_events = 0U;
        if (!WifiPort_Write((const uint8_t *)cmd, (uint16_t)strlen(cmd)))
        {
            if ((WifiPort_Now() - s_tx_started) >= 1000U)
            {
                fail("CIPSEND command TX queue full", 1U);
            }
            return;
        }
        s_tx_started = WifiPort_Now();
        s_tx_step = TX_PROMPT;
    }
    else if (s_tx_step == TX_PROMPT)
    {
        if (s_events & EV_PROMPT)
        {
            s_events = 0U;
            if (!WifiPort_Write(s_tx, s_tx_length))
            {
                /* 模块正在等待定长数据，队列满时下一轮继续重试。 */
                if ((WifiPort_Now() - s_tx_started) >= 2000U)
                {
                    fail("UDP payload TX queue full", 1U);
                }
                return;
            }
            s_tx_started = WifiPort_Now();
            s_tx_step = TX_ACK;
        }
        else if (s_events & EV_ERROR)
        {
            tx_next(0U);
        }
        else if ((WifiPort_Now() - s_tx_started) >= 3000U)
        {
            fail("CIPSEND prompt timeout", 1U);
        }
    }
    else if (s_tx_step == TX_ACK)
    {
        if (s_events & EV_SEND_OK)
        {
            tx_next(1U);
        }
        else if (s_events & EV_ERROR)
        {
            tx_next(0U);
        }
        else if ((WifiPort_Now() - s_tx_started) >= 6000U)
        {
            fail("SEND OK timeout (delivery unknown)", 1U);
        }
    }
}

void WifiAt_Poll(void)
{
    uint8_t byte;
    uint32_t received_ms;
    uint16_t i;
    char cmd[192];
    char msg[160];
    uint8_t optional, fallback;

    if (!s_initialized || (s_step == STEP_FAULT))
    {
        return;
    }

    /* 每次最多处理 256 字节，避免持续输入时主循环一直停在这里。 */
    for (i = 0U; i < 256U; ++i)
    {
        int result = WifiPort_Read(&byte, &received_ms);
        if (result == 0)
        {
            break;
        }
        if (result < 0)
        {
            parser_reset();
            /* ESP8266 上电/复位时 ROM 会以 74880 波特率打印启动信息，
             * 在 115200 下表现为帧错误；静默/复位阶段忽略，不能因此重试。 */
            if ((s_step == STEP_GUARD) || (s_step == STEP_ESCAPE) ||
                (s_step == STEP_RESET) || (s_step == STEP_RETRY))
            {
                continue;
            }
            fail("USART2 RX error/overflow", (uint8_t)(s_step == STEP_RUN));
            return;
        }
        (void)received_ms;
        ++s_stats.uart_rx_bytes;
        parse_byte(byte);
    }

    if (s_step == STEP_RETRY)
    {
        if ((WifiPort_Now() - s_started) >= WIFI_AT_RETRY_MS)
        {
            s_state = WIFI_AT_STARTING;
            parser_reset();
            change_step(STEP_GUARD);
        }
        return;
    }

    if (s_step == STEP_GUARD)
    {
        if ((WifiPort_Now() - s_started) >= 1100U)
        {
            if (!WifiPort_Write((const uint8_t *)"+++", 3U))
            {
                fail("UART escape TX queue full", 0U);
                return;
            }
            change_step(STEP_ESCAPE);
        }
        return;
    }

    if (s_step == STEP_ESCAPE)
    {
        if ((WifiPort_Now() - s_started) >= 1100U)
        {
            parser_reset();
            change_step(STEP_SYNC);
        }
        return;
    }

    if (s_step == STEP_RUN)
    {
        if (s_events & EV_READY)
        {
            s_events &= (uint8_t)~EV_READY;
            fail("module restarted", 0U);
            return;
        }
#if WIFI_NET_MODE == WIFI_NET_MODE_STA
        if (s_events & EV_DISCONNECT)
        {
            s_events &= (uint8_t)~EV_DISCONNECT;
            fail("WiFi disconnected", 0U);
            return;
        }
#endif
        send_poll();
        if (s_step != STEP_RUN)
        {
            /* 发送超时可能已经进入重试/故障状态，不能再把状态改回 READY。 */
            return;
        }
        if (!s_ready_logged)
        {
            s_ready_logged = 1U;
            s_state = WIFI_AT_READY;
#if WIFI_NET_MODE == WIFI_NET_MODE_AP
            snprintf(msg, sizeof(msg), "WiFi: AP \"%s\" ready, UDP %s:%u\r\n",
                     WIFI_AP_SSID, WIFI_BROADCAST_IP, (unsigned)WIFI_UDP_PORT);
#else
            snprintf(msg, sizeof(msg), "WiFi: STA connected, UDP %s:%u\r\n",
                     WIFI_BROADCAST_IP, (unsigned)WIFI_UDP_PORT);
#endif
            WifiPort_Log(msg);
        }
        return;
    }

    if (!s_waiting)
    {
        if (!command_for_step(cmd, sizeof(cmd)))
        {
            change_step((Step)(s_step + 1));
            return;
        }
        s_events = 0U;
        if (!WifiPort_Write((const uint8_t *)cmd, (uint16_t)strlen(cmd)))
        {
            if ((WifiPort_Now() - s_started) >= 1000U)
            {
                fail("AT command TX queue full", 0U);
            }
            return;
        }
        s_started = WifiPort_Now();
        s_waiting = 1U;
        return;
    }

    if (s_events & ((s_step == STEP_RESET) ? EV_READY : EV_OK))
    {
#if WIFI_NET_MODE == WIFI_NET_MODE_AP
        if (s_step == STEP_SOCKET)
        {
            /* 记录实际成功的广播地址；如果第一次 192.168.4.255 失败，
             * 这里会变成 255.255.255.255，CIPSEND 仍然发到广播。 */
            s_active_broadcast = (s_variant == 0U) ? WIFI_BROADCAST_IP : "255.255.255.255";
        }
#endif
        change_step((Step)(s_step + 1));
#if WIFI_NET_MODE == WIFI_NET_MODE_AP
        if (s_step == STEP_SOCKET)
        {
            /* CWSAP 成功后热点已经开始广播；先打印出来，方便电脑搜不到时判断
             * 是热点没建起来，还是后面的 UDP 建链失败。 */
            snprintf(msg, sizeof(msg), "WiFi: AP \"%s\" created (IP %s), opening UDP %s:%u...\r\n",
                     WIFI_AP_SSID, WIFI_AP_IP, s_active_broadcast, (unsigned)WIFI_UDP_PORT);
            WifiPort_Log(msg);
        }
#endif
        if (s_step == STEP_RUN)
        {
            s_events = 0U;
            s_state = WIFI_AT_READY;
            s_error = "none";
        }
        return;
    }

    if ((s_events & EV_ERROR) || ((WifiPort_Now() - s_started) >= s_timeout))
    {
        /* AT 失败可以重试；超时的指令可能仍在执行，直接进入重试等待。 */
        if ((s_step == STEP_SYNC) && (s_variant < 2U))
        {
            ++s_variant;
            s_waiting = 0U;
            parser_reset();
            return;
        }

        /* 有些旧 AT 固件会拒绝 192.168.4.255 这种子网广播，只认
         * 255.255.255.255。这里不重启模块，直接用全局广播再建一次 UDP，
         * 热点保持可见，也方便排查。 */
        if ((s_events & EV_ERROR) && (s_step == STEP_SOCKET) && (s_variant == 0U) &&
            (strcmp(WIFI_BROADCAST_IP, "255.255.255.255") != 0))
        {
            s_variant = 1U;
            s_waiting = 0U;
            parser_reset();
            WifiPort_Log("WiFi: CIPSTART broadcast rejected, retry 255.255.255.255\r\n");
            return;
        }

        optional = (uint8_t)((s_step == STEP_STORE) || (s_step == STEP_RECV_MODE) ||
                             (s_step == STEP_CLEAR_AUTO) || (s_step == STEP_AP_IP) ||
                             (s_step == STEP_DHCP));
        fallback = (uint8_t)((s_step == STEP_MODE) || (s_step == STEP_AP_IP) ||
                             (s_step == STEP_NETWORK));
        if ((s_events & EV_ERROR) && fallback && (s_variant == 0U))
        {
            s_variant = 1U;
            s_waiting = 0U;
        }
        else if ((s_events & EV_ERROR) && (s_step == STEP_DHCP) && (s_variant < 2U))
        {
            ++s_variant;
            s_waiting = 0U;
        }
        else if ((s_events & EV_ERROR) && optional)
        {
            change_step((Step)(s_step + 1));
        }
        else
        {
            fail(step_name(), 0U);
        }
    }
}

uint8_t WifiAt_Send(const uint8_t *data, uint16_t length)
{
    if ((s_state != WIFI_AT_READY) || (s_step != STEP_RUN) || (s_tx_step != TX_IDLE))
    {
        ++s_stats.tx_busy;
        return 0U;
    }
    if ((data == NULL) || (length == 0U) || (length > sizeof(s_tx)))
    {
        return 0U;
    }
    memcpy(s_tx, data, length);
    s_tx_length = length;
    s_tx_started = WifiPort_Now();
    s_tx_step = TX_BEGIN;
    return 1U;
}

uint16_t WifiAt_Read(uint8_t *data, uint16_t capacity)
{
    uint16_t n = 0U;
    if (data == NULL)
    {
        return 0U;
    }
    while ((n < capacity) && (s_rx_read != s_rx_write))
    {
        data[n++] = s_rx[s_rx_read];
        s_rx_read = (uint16_t)((s_rx_read + 1U) & (WIFI_RX_RING_SIZE - 1U));
    }
    s_stats.rx_bytes += n;
    return n;
}

uint16_t WifiAt_Available(void)
{
    return (uint16_t)((s_rx_write - s_rx_read) & (WIFI_RX_RING_SIZE - 1U));
}

WifiAtState WifiAt_GetState(void)
{
    return s_state;
}

uint8_t WifiAt_IsReady(void)
{
    return (uint8_t)((s_state == WIFI_AT_READY) && (s_step == STEP_RUN));
}

const char *WifiAt_LastError(void)
{
    return s_error;
}

const char *WifiAt_StateName(void)
{
    switch (s_state)
    {
        case WIFI_AT_OFF: return "RETRY";
        case WIFI_AT_STARTING: return "STARTING";
        case WIFI_AT_READY: return "READY";
        case WIFI_AT_ERROR: return "ERROR";
        default: return "?";
    }
}

const char *WifiAt_GetBroadcastIp(void)
{
    return s_active_broadcast;
}

const WifiAtStats *WifiAt_GetStats(void)
{
    return &s_stats;
}

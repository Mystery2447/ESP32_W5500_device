#include <stdio.h>
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "my_cmd.h"
#include "my_nvs.h"
#include "eth_init.h"           /* 替代 wifi_sta.h */
#include "relay.h"
#include "esp_console.h"
#include "esp_system.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/sys.h"
#include <errno.h>

#define SOCU_UART_TX  GPIO_NUM_18
#define SOCU_UART_RX  GPIO_NUM_19
#define MCU_UART_TX  GPIO_NUM_17
#define MCU_UART_RX  GPIO_NUM_5
#define TCP_SERVER_PORT   8080
#define TERMINAL_CONSOLE  "sock_terminal>"
#define UART_TX   MCU_UART_TX
#define UART_RX   MCU_UART_RX
#define USE_UART  UART_NUM_1
#define SOCU_UART UART_NUM_2
#define UART_BUF_SIZE 512

#define PRIORITY_TCP_LISTEN  3
#define PRIORITY_TCP_CLIENT  4
#define PRIORITY_UART_TX     5
#define PRIORITY_UART_RX     6

#define CORE_0  0
#define CORE_1  1

int   tcp_server_init(void);
void  check_client_full(void);
esp_err_t uart_init(void);
esp_err_t uart_socu_init(void);
void  tcp_client_handle_task(void *arg);
void  tcp_server_listen_task(void *arg);
void  uart_transfer_task(void *arg);
void  uart_socu_transfer_task(void *arg);

static const char *TAG = "MAIN";
static SemaphoreHandle_t s_tcp_client_sem  = NULL;
static SemaphoreHandle_t s_mcu_uart_mutex  = NULL;  /* MCU UART 互斥锁 */
static SemaphoreHandle_t s_socu_uart_mutex = NULL;  /* SOCU UART 互斥锁 */
static QueueHandle_t     uart_to_tcp_queue = NULL;

IP4_info_t net = {0};   /* 全局网络配置，my_nvs 通过 get_eth_netif() 访问 */

typedef struct {
    uint8_t *data;
    size_t   len;
} serial_data_t;

typedef struct {
    int                socket_fd;
    struct sockaddr_in client_addr;
    bool               en_read_uart;       /* 本连接是否在 MCU UART 透传模式 */
    bool               en_read_socu_uart;  /* 本连接是否在 SOCU UART 透传模式 */
    bool               owns_mcu_uart;      /* 本连接是否持有 MCU UART 锁 */
    bool               owns_socu_uart;     /* 本连接是否持有 SOCU UART 锁 */
    int                exit_match;         /* SOCU 字符模式 exit() 状态机 */
    bool               disconnected;       /* 由 transfer task 设置，标记连接已断开 */
} client_info_t;

esp_console_repl_t *repl_env = NULL;

/* ------------------------------------------------------------------ */
esp_err_t uart_init(void)
{
    uart_config_t uart_config = {
        .baud_rate  = 115200,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(USE_UART, &uart_config);
    uart_set_pin(USE_UART, UART_TX, UART_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    return uart_driver_install(USE_UART, UART_BUF_SIZE, UART_BUF_SIZE, 0, NULL, 0);
}

/* ------------------------------------------------------------------ */
esp_err_t uart_socu_init(void)
{
    uart_config_t uart_config = {
        .baud_rate  = 115200,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_param_config(SOCU_UART, &uart_config);
    esp_err_t pin_err = uart_set_pin(SOCU_UART, SOCU_UART_TX, SOCU_UART_RX,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    ESP_LOGI(TAG, "socu_uart_set_pin(TX=%d,RX=%d) returned 0x%x",
             SOCU_UART_TX, SOCU_UART_RX, pin_err);
    int rx_level = gpio_get_level(SOCU_UART_RX);
    ESP_LOGI(TAG, "socu_uart RX pin GPIO_%d level=%d", SOCU_UART_RX, rx_level);
    esp_err_t err = uart_driver_install(SOCU_UART, UART_BUF_SIZE, UART_BUF_SIZE, 0, NULL, 0);
    if (err != ESP_OK) return err;
    return ESP_OK;
}

void repl_init(void)
{
    esp_console_dev_uart_config_t dev_init  = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    esp_console_repl_config_t     repl_init = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_init.prompt = "esp32_zfc>";
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&dev_init, &repl_init, &repl_env));
}

void repl_start(void)
{
    esp_console_start_repl(repl_env);
    printf("Console started. Try 'help'.\n");
}

/* ------------------------------------------------------------------ */
void app_main(void)
{
    init_nvs();

    if (uart_init() != ESP_OK) {
        ESP_LOGE(TAG, "uart init fail...");
        return;
    }

    if (uart_socu_init() != ESP_OK) {
        ESP_LOGE(TAG, "socu uart init fail...");
        return;
    }

    /* 初始化网络协议栈和事件循环（替代 wifi_netif_init）*/
    eth_netif_init();

    /* 从NVS读取静态IP配置；若没有则默认DHCP */
    esp_err_t ret = get_network_info(&net);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "no network config in NVS, using DHCP");
        net.dhcp = 1;
    }

    /* 启动W5500以太网（替代 wifi_init_sta）*/
    eth_start(&net);

    s_tcp_client_sem = xSemaphoreCreateCounting(3, 3);
    if (s_tcp_client_sem == NULL) {
        ESP_LOGE(TAG, "create tcp client semaphore fail...");
        return;
    }

    s_mcu_uart_mutex = xSemaphoreCreateMutex();
    s_socu_uart_mutex = xSemaphoreCreateMutex();

    uart_to_tcp_queue = xQueueCreate(20, sizeof(serial_data_t));
    repl_init();
    register_my_cmd();
    repl_start();

    xTaskCreatePinnedToCore(tcp_server_listen_task,
                            "tcp_listen",
                            4096,
                            NULL,
                            PRIORITY_TCP_LISTEN,
                            NULL,
                            CORE_0);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ------------------------------------------------------------------ */
void tcp_server_listen_task(void *arg)
{
    char addr_str[INET_ADDRSTRLEN];
    int  listen_sock = tcp_server_init();
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "tcp server init fail...");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "tcp server init success...");

    for (;;) {
        ESP_LOGI(TAG, "waiting for client to connect...");
        check_client_full();

        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        int client_sock = accept(listen_sock, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_sock < 0) {
            ESP_LOGI(TAG, "accept client fail...");
            xSemaphoreGive(s_tcp_client_sem);
            continue;
        }

        client_info_t *client_info = malloc(sizeof(client_info_t));
        if (client_info == NULL) {
            ESP_LOGE(TAG, "Failed to allocate client info");
            close(client_sock);
            xSemaphoreGive(s_tcp_client_sem);
            continue;
        }

        client_info->socket_fd = client_sock;
        client_info->en_read_uart = false;
        client_info->en_read_socu_uart = false;
        client_info->owns_mcu_uart = false;
        client_info->owns_socu_uart = false;
        client_info->exit_match = 0;
        client_info->disconnected = false;
        memcpy(&client_info->client_addr, &client_addr, sizeof(client_addr));
        inet_ntoa_r(client_addr.sin_addr, addr_str, sizeof(addr_str) - 1);
        ESP_LOGI(TAG, "client ip:%s port:%d", addr_str, ntohs(client_addr.sin_port));

        if (xTaskCreatePinnedToCore(tcp_client_handle_task,
                                    "tcp_client",
                                    4096,
                                    (void *)client_info,
                                    PRIORITY_TCP_CLIENT,
                                    NULL,
                                    CORE_1) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create client task");
            free(client_info);
            close(client_sock);
            xSemaphoreGive(s_tcp_client_sem);
        }
    }

    close(listen_sock);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
void uart_transfer_task(void *pvParameter)
{
    client_info_t *client_info = (client_info_t *)pvParameter;
    int tcp_sock = client_info->socket_fd;
    uint8_t buff[UART_BUF_SIZE];

    while (1) {
        if (!client_info->en_read_uart) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        int len = uart_read_bytes(USE_UART, buff, sizeof(buff), pdMS_TO_TICKS(10));
        if (len > 0) {
            uint8_t *data_send = buff;
            int remain_len = len;
            while (remain_len > 0) {
                int send_len = send(tcp_sock, data_send, remain_len, 0);
                if (send_len < 0) {
                    ESP_LOGE(TAG, "tcp send fail, signaling disconnect...");
                    client_info->disconnected = true;
                    vTaskSuspend(NULL);  /* 挂起自己，等待父任务统一清理 */
                }
                data_send  += send_len;
                remain_len -= send_len;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
void uart_socu_transfer_task(void *pvParameter)
{
    client_info_t *client_info = (client_info_t *)pvParameter;
    int tcp_sock = client_info->socket_fd;
    uint8_t buff[UART_BUF_SIZE];
    bool was_active = false;
    int poll_count = 0;

    while (1) {
        if (!client_info->en_read_socu_uart) {
            was_active = false;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (!was_active) {
            int baud = 0;
            uart_get_baudrate(SOCU_UART, &baud);
            size_t rx_buffered = 0;
            uart_get_buffered_data_len(SOCU_UART, &rx_buffered);
            ESP_LOGI(TAG, "socu_uart task active, baud=%d, rx_buffered=%d, polling UART...",
                     baud, (int)rx_buffered);
            was_active = true;
            poll_count = 0;
        }
        int len = uart_read_bytes(SOCU_UART, buff, sizeof(buff), pdMS_TO_TICKS(50));
        if (len > 0) {
            ESP_LOGI(TAG, "socu_uart read %d bytes", len);
            poll_count = 0;
            uint8_t *data_send = buff;
            int remain_len = len;
            while (remain_len > 0) {
                int send_len = send(tcp_sock, data_send, remain_len, 0);
                if (send_len < 0) {
                    ESP_LOGE(TAG, "socu_uart send fail, errno=%d", errno);
                    client_info->disconnected = true;
                    vTaskSuspend(NULL);
                }
                data_send  += send_len;
                remain_len -= send_len;
            }
            ESP_LOGI(TAG, "socu_uart sent %d bytes to client", len);
        } else {
            poll_count++;
            if (poll_count % 20 == 1) {
                size_t rx_buf = 0;
                uart_get_buffered_data_len(SOCU_UART, &rx_buf);
                ESP_LOGI(TAG, "socu_uart polling... (no data, count=%d, rx_buf=%d)",
                         poll_count, (int)rx_buf);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Telnet IAC filter: 剥离 Telnet 协议转义，响应协商，Ctrl+C → 0x03     */
/* 返回过滤后的有效数据长度，数据在 buf 中原地修改                      */
static int telnet_filter(int sock, char *buf, int len, bool in_socu_mode)
{
    /* 本服务器支持的 option */
    static const unsigned char supported[] = {0, 1, 3};
    #define NUM_SUP (sizeof(supported) / sizeof(supported[0]))

    int w = 0;
    for (int r = 0; r < len; r++) {
        if (buf[r] == '\xFF' && r + 1 < len) {
            r++;  /* 跳过 IAC */
            switch ((unsigned char)buf[r]) {
            case '\xFF':  /* 转义的字面 0xFF */
                buf[w++] = '\xFF';
                break;
            case '\xFD':  /* IAC DO <opt> */
                if (r + 1 < len) {
                    unsigned char opt = buf[r + 1];
                    int ok = 0;
                    for (int i = 0; i < NUM_SUP; i++) { if (supported[i] == opt) ok = 1; }
                    char resp[3] = {'\xFF', ok ? '\xFB' : '\xFC', opt};
                    send(sock, resp, 3, 0);
                    r++;
                }
                break;
            case '\xFB':  /* IAC WILL <opt> */
                if (r + 1 < len) {
                    unsigned char opt = buf[r + 1];
                    int ok = 0;
                    for (int i = 0; i < NUM_SUP; i++) { if (supported[i] == opt) ok = 1; }
                    /* SOCU 模式下拒绝客户端 Echo：服务器负责回显 */
                    if (in_socu_mode && opt == 1) ok = 0;
                    char resp[3] = {'\xFF', ok ? '\xFD' : '\xFE', opt};
                    send(sock, resp, 3, 0);
                    r++;
                }
                break;
            case '\xFA':  /* IAC SB 子协商 → 跳过直到 IAC SE */
                while (r + 1 < len && !(buf[r] == '\xFF' && buf[r + 1] == '\xF0')) {
                    r++;
                }
                if (r + 1 < len) r++;  /* 跳过 SE */
                break;
            case '\xF4':  /* IAC IP (Interrupt Process) → Ctrl+C */
            case '\xF3':  /* IAC BRK */
                buf[w++] = '\x03';
                break;
            case '\xF7':  /* IAC EC (Erase Character) → Backspace */
                buf[w++] = '\x08';
                break;
            case '\xF8':  /* IAC EL (Erase Line) → Ctrl+U */
                buf[w++] = '\x15';
                break;
            case '\xFE':  /* IAC DONT */
            case '\xFC':  /* IAC WONT */
                if (r + 1 < len) r++;  /* 跳过 option 字节 */
                break;
            case '\xF0':  /* IAC SE */
            case '\xF1':  /* IAC NOP */
            case '\xF2':  /* IAC DM */
            case '\xF5':  /* IAC AO */
            case '\xF6':  /* IAC AYT */
            case '\xF9':  /* IAC GA */
                break;  /* 静默忽略 */
            default:
                break;  /* 未知 IAC 命令，忽略 */
            }
        } else if (buf[r] == '\xFF') {
            /* IAC 在缓冲区末尾，丢弃（不完整序列） */
        } else {
            buf[w++] = buf[r];
        }
    }
    return w;
}

/* ------------------------------------------------------------------ */
void tcp_client_handle_task(void *arg)
{
    client_info_t *client_info = (client_info_t *)arg;
    int client_sock = client_info->socket_fd;
    char rd_buff[512] = {0};
    int  cnt;

    /* 设置 socket 超时，防止 recv/send 永久阻塞导致信号量泄漏 */
    struct timeval tv = {.tv_sec = 60, .tv_usec = 0};
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(client_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    /* 启用 TCP keepalive，快速检测死连接 */
    int keepalive = 1;
    setsockopt(client_sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
#ifdef TCP_KEEPIDLE
    int keepidle  = 10;   /* 空闲 10 秒后开始探测 */
    int keepintvl = 5;    /* 探测间隔 5 秒 */
    int keepcnt   = 3;    /* 最多 3 次探测，总计 10+3*5=25 秒判定死亡 */
    setsockopt(client_sock, IPPROTO_TCP, TCP_KEEPIDLE,  &keepidle,  sizeof(keepidle));
    setsockopt(client_sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
    setsockopt(client_sock, IPPROTO_TCP, TCP_KEEPCNT,   &keepcnt,   sizeof(keepcnt));
#endif

    send(client_sock, "welcome to esp32 tcp server\r\n",      30, 0);
    send(client_sock, "type 'exit()' to disconnect\r\n",      30, 0);
    send(client_sock, "type 'relay <1|2> <on|off>' to control relay\r\n", 47, 0);
    send(client_sock, "type 'read_mcu_uart' to read mcu uart data\r\n", 44, 0);
    send(client_sock, "type 'read_soc_uart' to read soc uart data\r\n", 47, 0);
    send(client_sock, "type 'reset_esp32' to restart ESP32\r\n",      38, 0);

    /* 不主动协商字符模式，保持行模式用于命令输入 */
    /* 进入 UART 透传模式时才协商 Echo + Suppress Go-Ahead */

    /* 创建 UART 转发任务，常驻运行，由 client_info->en_read_uart 控制是否转发 */
    TaskHandle_t uart_task = NULL;
    xTaskCreatePinnedToCore(uart_transfer_task,
                            "uart_transfer",
                            4096,
                            (void *)client_info,
                            PRIORITY_UART_RX,
                            &uart_task,
                            CORE_1);

    /* 创建 SOCU UART 转发任务 */
    TaskHandle_t socu_uart_task = NULL;
    xTaskCreatePinnedToCore(uart_socu_transfer_task,
                            "socu_uart_transfer",
                            4096,
                            (void *)client_info,
                            PRIORITY_UART_RX,
                            &socu_uart_task,
                            CORE_1);

    char cmd_buf[512] = {0};  /* 命令模式行缓冲 */
    int  cmd_pos = 0;

    for (;;) {
        cnt = recv(client_sock, rd_buff, sizeof(rd_buff) - 1, 0);
        if (cnt < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* 超时，检查 transfer task 是否已标记断开 */
                if (client_info->disconnected) {
                    ESP_LOGI(TAG, "transfer task signaled disconnect, cleaning up...");
                    break;
                }
                continue;  /* 普通超时，继续等待 */
            }
            ESP_LOGE(TAG, "recv fail, disconnecting...");
            break;
        } else if (cnt == 0) {
            ESP_LOGI(TAG, "client disconnected...");
            break;
        }
        rd_buff[cnt] = '\0';

        /* 调试：打印原始TCP数据的十六进制和ASCII */
        {
            char hex_line[128];
            int pos = snprintf(hex_line, sizeof(hex_line), "recv raw [%d]: ", cnt);
            for (int i = 0; i < cnt && pos < (int)sizeof(hex_line) - 4; i++) {
                pos += snprintf(hex_line + pos, sizeof(hex_line) - pos,
                                "%02X ", (unsigned char)rd_buff[i]);
            }
            ESP_LOGI(TAG, "%s", hex_line);

            char ascii_line[128];
            pos = snprintf(ascii_line, sizeof(ascii_line), "recv asc [%d]: ", cnt);
            for (int i = 0; i < cnt && pos < (int)sizeof(ascii_line) - 4; i++) {
                unsigned char c = rd_buff[i];
                pos += snprintf(ascii_line + pos, sizeof(ascii_line) - pos, "%c",
                                (c >= 0x20 && c < 0x7F) ? c : '.');
            }
            ESP_LOGI(TAG, "%s", ascii_line);
        }

        /* Telnet IAC 过滤：剥离转义序列，Ctrl+C → 0x03 */
        cnt = telnet_filter(client_sock, rd_buff, cnt, client_info->en_read_socu_uart);
        if (cnt == 0) {
            continue;  /* 全是 Telnet 命令，无有效数据 */
        }

        /* ── SOCU UART 透传模式：字符模式，逐字转发，检测 exit() 退出 ── */
        if (client_info->en_read_socu_uart) {
            static const char exit_seq[] = "exit()\r";

            int wr = uart_write_bytes(SOCU_UART, rd_buff, cnt);
            {
                size_t tx_free = 0;
                uart_get_tx_buffer_free_size(SOCU_UART, &tx_free);
                ESP_LOGI(TAG, "socu_uart write %d/%d bytes, tx_free=%d", wr, cnt, (int)tx_free);
            }
            /* 不在此处回显：串口对端设备会回显，通过 uart_socu_transfer_task 传回 */

            for (int i = 0; i < cnt; i++) {
                if (rd_buff[i] == exit_seq[client_info->exit_match]) {
                    client_info->exit_match++;
                    if (client_info->exit_match == 7) {
                        client_info->en_read_socu_uart = false;
                        client_info->exit_match = 0;
                        if (client_info->owns_socu_uart) {
                            client_info->owns_socu_uart = false;
                            xSemaphoreGive(s_socu_uart_mutex);
                        }
                        cmd_pos = 0;
                        {
                            const char unneg[] = {
                                '\xFF','\xFC','\x03',  /* IAC WONT Suppress Go-Ahead */
                                '\xFF','\xFC','\x01',  /* IAC WONT Echo */
                            };
                            send(client_sock, unneg, sizeof(unneg), 0);
                        }
                        send(client_sock, "\r\nwelcome to esp32 tcp server\r\n",       30, 0);
                        send(client_sock, "type 'exit()' to disconnect\r\n",       30, 0);
                        send(client_sock, "type 'relay <1|2> <on|off>' to control relay\r\n", 47, 0);
                        send(client_sock, "type 'read_mcu_uart' to read mcu uart data\r\n", 44, 0);
                        send(client_sock, "type 'read_soc_uart' to read soc uart data\r\n", 47, 0);
    send(client_sock, "type 'reset_esp32' to restart ESP32\r\n",      38, 0);
                    }
                } else {
                    client_info->exit_match = 0;
                }
            }
            continue;
        }

        /* ── MCU UART 透传模式：行模式，缓冲整行后发送 ── */
        if (client_info->en_read_uart) {
            if (cmd_pos + cnt >= (int)sizeof(cmd_buf) - 1) {
                cmd_pos = 0;  /* 溢出，重置 */
            }
            memcpy(cmd_buf + cmd_pos, rd_buff, cnt);
            cmd_pos += cnt;
            cmd_buf[cmd_pos] = '\0';

            if (strchr(cmd_buf, '\n') == NULL) {
                continue;
            }

            if (strcmp(cmd_buf, "exit()\r\n") == 0) {
                client_info->en_read_uart = false;
                if (client_info->owns_mcu_uart) {
                    client_info->owns_mcu_uart = false;
                    xSemaphoreGive(s_mcu_uart_mutex);
                }
                cmd_pos = 0;
                send(client_sock, "\r\nwelcome to esp32 tcp server\r\n",       30, 0);
                send(client_sock, "type 'exit()' to disconnect\r\n",       30, 0);
                send(client_sock, "type 'relay <1|2> <on|off>' to control relay\r\n", 47, 0);
                send(client_sock, "type 'read_mcu_uart' to read mcu uart data\r\n", 44, 0);
                send(client_sock, "type 'read_soc_uart' to read soc uart data\r\n", 47, 0);
    send(client_sock, "type 'reset_esp32' to restart ESP32\r\n",      38, 0);
            } else {
                uart_write_bytes(USE_UART, cmd_buf, cmd_pos);
                cmd_pos = 0;
            }
            continue;
        }

        /* ── 命令模式：行缓冲拼完整行后匹配 ── */
        if (cmd_pos + cnt >= (int)sizeof(cmd_buf) - 1) {
            cmd_pos = 0;  /* 溢出，重置 */
        }
        memcpy(cmd_buf + cmd_pos, rd_buff, cnt);
        cmd_pos += cnt;
        cmd_buf[cmd_pos] = '\0';

        /* 检查是否有完整行（以 \n 结尾） */
        if (strchr(cmd_buf, '\n') == NULL) {
            continue;
        }

        if (strcmp(cmd_buf, "exit()\r\n") == 0) {
            send(client_sock, TERMINAL_CONSOLE "disconnecting...\r\n", 18, 0);
            break;
        } else if (strcmp(cmd_buf, "read_mcu_uart\r\n") == 0) {
            if (client_info->en_read_uart) {
                send(client_sock, "already in UART transfer mode\r\n", 31, 0);
            } else if (xSemaphoreTake(s_mcu_uart_mutex, 0) != pdTRUE) {
                send(client_sock, "MCU UART is busy, another client is using it\r\n", 48, 0);
            } else {
                client_info->owns_mcu_uart = true;
                client_info->en_read_uart = true;
                send(client_sock, "enter UART transfer success...\r\n", 33, 0);
            }
        } else if (strcmp(cmd_buf, "read_soc_uart\r\n") == 0) {
            if (client_info->en_read_socu_uart) {
                send(client_sock, "already in SOCU UART transfer mode\r\n", 37, 0);
            } else if (xSemaphoreTake(s_socu_uart_mutex, 0) != pdTRUE) {
                send(client_sock, "SOCU UART is busy, another client is using it\r\n", 49, 0);
            } else {
                if (client_info->en_read_uart) {
                    client_info->en_read_uart = false;
                    if (client_info->owns_mcu_uart) {
                        client_info->owns_mcu_uart = false;
                        xSemaphoreGive(s_mcu_uart_mutex);
                    }
                }
                client_info->owns_socu_uart = true;
                client_info->en_read_socu_uart = true;
                ESP_LOGI(TAG, "entering SOCU UART mode");
                /* 进入字符模式：协商 Echo + Suppress Go-Ahead */
                /* 注意：只协商不回显，回显由串口对端设备通过 uart_socu_transfer_task 传回 */
                {
                    const char neg[] = {
                        '\xFF','\xFB','\x03',  /* IAC WILL Suppress Go-Ahead */
                        '\xFF','\xFB','\x01',  /* IAC WILL Echo — 禁止客户端本地回显 */
                    };
                    send(client_sock, neg, sizeof(neg), 0);
                }
                send(client_sock, "enter SOCU UART transfer success...\r\n", 39, 0);
            }
        } else if (strncmp(cmd_buf, "relay ", 6) == 0) {
            /* 格式: relay <1|2> <on|off>\r\n */
            int r_num;
            char r_state[8] = {0};
            if (sscanf(cmd_buf, "relay %d %7s", &r_num, r_state) == 2) {
                if (r_num < 1 || r_num > 2) {
                    send(client_sock, "invalid relay number (use 1 or 2)\r\n", 37, 0);
                } else if (strcmp(r_state, "on") == 0) {
                    relay_set(r_num, true);
                    char resp[32];
                    int len = snprintf(resp, sizeof(resp), "Relay%d ON\r\n", r_num);
                    send(client_sock, resp, len, 0);
                } else if (strcmp(r_state, "off") == 0) {
                    relay_set(r_num, false);
                    char resp[32];
                    int len = snprintf(resp, sizeof(resp), "Relay%d OFF\r\n", r_num);
                    send(client_sock, resp, len, 0);
                } else {
                    send(client_sock, "invalid state (use on/off)\r\n", 29, 0);
                }
            } else {
                send(client_sock, "usage: relay <1|2> <on|off>\r\n", 31, 0);
            }
        } else if (strcmp(cmd_buf, "reset_esp32\r\n") == 0) {
            send(client_sock, "Resetting ESP32...\r\n", 21, 0);
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
        cmd_pos = 0;  /* 处理完一行，重置缓冲 */
    }

    close(client_sock);
    client_info->en_read_uart = false;
    client_info->en_read_socu_uart = false;
    if (client_info->owns_mcu_uart) {
        client_info->owns_mcu_uart = false;
        xSemaphoreGive(s_mcu_uart_mutex);
    }
    if (client_info->owns_socu_uart) {
        client_info->owns_socu_uart = false;
        xSemaphoreGive(s_socu_uart_mutex);
    }
    if (uart_task != NULL) {
        vTaskDelete(uart_task);
    }
    if (socu_uart_task != NULL) {
        vTaskDelete(socu_uart_task);
    }
    free(client_info);
    if (xSemaphoreGive(s_tcp_client_sem) != pdTRUE) {
        ESP_LOGE(TAG, "semaphore give fail...");
    }
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
void check_client_full(void)
{
    if (xSemaphoreTake(s_tcp_client_sem, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "client full, semaphore take fail...");
    }
}

int tcp_server_init(void)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        ESP_LOGE(TAG, "create socket fail...");
        return -1;
    }
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char[]){1}, sizeof(int));
    struct sockaddr_in dest_addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(TCP_SERVER_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    dest_addr.sin_len = sizeof(dest_addr);
    if (bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) < 0) {
        ESP_LOGE(TAG, "bind fail...");
        close(sock);
        return -1;
    }
    if (listen(sock, 3) < 0) {
        ESP_LOGE(TAG, "listen fail...");
        close(sock);
        return -1;
    }
    return sock;
}

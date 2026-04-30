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
#include "esp_console.h"
#include "esp_system.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/sys.h"

#define SOCU_UART_TX GPIO_NUM_17
#define SOCU_UART_RX GPIO_NUM_16
#define MCU_UART_TX  GPIO_NUM_32
#define MCU_UART_RX  GPIO_NUM_33
#define TCP_SERVER_PORT   8080
#define TERMINAL_CONSOLE  "sock_terminal>"
#define UART_TX   MCU_UART_TX
#define UART_RX   MCU_UART_RX
#define USE_UART  UART_NUM_1
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
void  tcp_client_handle_task(void *arg);
void  tcp_server_listen_task(void *arg);

static const char *TAG = "MAIN";
static SemaphoreHandle_t s_tcp_client_sem  = NULL;
static QueueHandle_t     uart_to_tcp_queue = NULL;

static volatile bool en_read_uart  = false;
static TaskHandle_t  xUartTransferTask = NULL;

IP4_info_t net = {0};   /* 全局网络配置，my_nvs 通过 get_eth_netif() 访问 */

typedef struct {
    uint8_t *data;
    size_t   len;
} serial_data_t;

typedef struct {
    int                socket_fd;
    struct sockaddr_in client_addr;
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
        int len = uart_read_bytes(USE_UART, buff, sizeof(buff), pdMS_TO_TICKS(10));
        if (len > 0) {
            uint8_t *data_send = buff;
            int remain_len = len;
            while (remain_len > 0) {
                int send_len = send(tcp_sock, data_send, remain_len, 0);
                if (send_len < 0) {
                    ESP_LOGE(TAG, "tcp send fail, disconnecting...");
                    close(tcp_sock);
                    free(client_info);
                    vTaskDelete(NULL);
                }
                data_send  += send_len;
                remain_len -= send_len;
            }
        }
    }

    close(tcp_sock);
    free(client_info);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
void tcp_client_handle_task(void *arg)
{
    client_info_t *client_info = (client_info_t *)arg;
    int client_sock = client_info->socket_fd;
    char rd_buff[512] = {0};
    int  cnt;

    send(client_sock, "welcome to esp32 tcp server\r\n",      30, 0);
    send(client_sock, "type 'exit()' to disconnect\r\n",      30, 0);
    send(client_sock, "type 'read_uart' to read uart data\r\n", 40, 0);

    for (;;) {
        cnt = recv(client_sock, rd_buff, sizeof(rd_buff) - 1, 0);
        if (cnt < 0) {
            ESP_LOGE(TAG, "recv fail, disconnecting...");
            break;
        } else if (cnt == 0) {
            ESP_LOGI(TAG, "client disconnected...");
            break;
        }
        rd_buff[cnt] = '\0';
        ESP_LOGI(TAG, "recv %d bytes: %s", cnt, rd_buff);

        if (strcmp(rd_buff, "exit()\r\n") == 0) {
            if (en_read_uart) {
                en_read_uart = false;
                if (xUartTransferTask != NULL) {
                    vTaskDelete(xUartTransferTask);
                    xUartTransferTask = NULL;
                    send(client_sock, "welcome to esp32 tcp server\r\n",       30, 0);
                    send(client_sock, "type 'exit()' to disconnect\r\n",       30, 0);
                    send(client_sock, "type 'read_uart' to read uart data\r\n", 40, 0);
                }
            } else {
                send(client_sock, TERMINAL_CONSOLE "disconnecting...\r\n", 18, 0);
                break;
            }
        } else if (strcmp(rd_buff, "read_mcu_uart\r\n") == 0 ||
                   strcmp(rd_buff, "read_uart\n") == 0) {
            if (en_read_uart) continue;
            en_read_uart = true;
            if (xUartTransferTask == NULL) {
                xTaskCreatePinnedToCore(uart_transfer_task,
                                        "uart_transfer_task",
                                        4096,
                                        (void *)client_info,
                                        PRIORITY_UART_RX,
                                        &xUartTransferTask,
                                        CORE_1);
                send(client_sock, "enter UART transfer success...\r\n", 33, 0);
            } else {
                ESP_LOGW(TAG, "UART task already running");
            }
        } else if (en_read_uart) {
            uart_write_bytes(USE_UART, rd_buff, strlen(rd_buff));
        }
    }

    close(client_sock);
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

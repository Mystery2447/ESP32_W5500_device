#include "eth_init.h"
#include "esp_log.h"
#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_phy.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include <string.h>
static const char *TAG = "ETH_INIT";

/* W5500 SPI 帧：[addr_hi, addr_lo, ctrl, data]
 * 读公共寄存器块：ctrl = 0x00，VERSIONR 地址 = 0x0039，应返回 0x04 */
static void w5500_spi_check(void)
{
    spi_device_handle_t spi;
    spi_device_interface_config_t devcfg = {
        .mode           = 0,
        .clock_speed_hz = 5 * 1000 * 1000,   /* 5MHz，最保守速率 */
        .spics_io_num   = ETH_SPI_CS_GPIO,
        .queue_size     = 1,
    };
    if (spi_bus_add_device(ETH_SPI_HOST, &devcfg, &spi) != ESP_OK) {
        ESP_LOGE(TAG, "SPI check: add device failed");
        return;
    }

    uint8_t tx[4] = {0x00, 0x39, 0x00, 0x00};  /* 读 VERSIONR */
    uint8_t rx[4] = {0};
    spi_transaction_t t = {
        .length    = 32,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    if (spi_device_transmit(spi, &t) == ESP_OK) {
        ESP_LOGI(TAG, "W5500 VERSIONR = 0x%02X (expect 0x04)", rx[3]);
        if (rx[3] == 0x04) {
            ESP_LOGI(TAG, "SPI communication OK");
        } else if (rx[3] == 0xFF) {
            ESP_LOGE(TAG, "SPI communication FAIL: MISO stuck high (check wiring)");
        } else if (rx[3] == 0x00) {
            ESP_LOGE(TAG, "SPI communication FAIL: MISO stuck low (check wiring)");
        } else {
            ESP_LOGE(TAG, "SPI communication FAIL: unexpected value");
        }
    } else {
        ESP_LOGE(TAG, "SPI check: transmit failed");
    }

    spi_bus_remove_device(spi);
}

static esp_netif_t         *s_eth_netif      = NULL;
static EventGroupHandle_t   s_eth_event_grp;
static IP4_info_t           s_net_cfg;        /* 本地保存静态IP配置 */

#define ETH_CONNECTED_BIT  BIT0
#define ETH_FAIL_BIT       BIT1

/* ------------------------------------------------------------------ */
static void eth_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == ETH_EVENT) {
        switch (event_id) {
        case ETHERNET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Ethernet Link Up");
            if (s_net_cfg.dhcp == 0) {
                /* 静态IP：停掉DHCP客户端，手动设置地址 */
                esp_netif_dhcpc_stop(s_eth_netif);
                esp_netif_ip_info_t ip_info = {};
                ip_info.ip.addr      = esp_ip4addr_aton(s_net_cfg.ip);
                ip_info.gw.addr      = esp_ip4addr_aton(s_net_cfg.gateway);
                ip_info.netmask.addr = esp_ip4addr_aton(s_net_cfg.netmask);
                if (esp_netif_set_ip_info(s_eth_netif, &ip_info) != ESP_OK) {
                    ESP_LOGE(TAG, "Set static IP failed");
                } else {
                    ESP_LOGI(TAG, "Static IP: %s", s_net_cfg.ip);
                    xEventGroupSetBits(s_eth_event_grp, ETH_CONNECTED_BIT);
                }
            } else {
                /* DHCP：显式启动DHCP客户端 */
                ESP_LOGI(TAG, "Starting DHCP client...");
                esp_netif_dhcpc_start(s_eth_netif);
            }
            break;

        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Ethernet Link Down");
            esp_netif_dhcpc_stop(s_eth_netif);
            xEventGroupSetBits(s_eth_event_grp, ETH_FAIL_BIT);
            break;

        case ETHERNET_EVENT_START:
            ESP_LOGI(TAG, "Ethernet Started");
            break;

        case ETHERNET_EVENT_STOP:
            ESP_LOGI(TAG, "Ethernet Stopped");
            break;

        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_ETH_GOT_IP) {
        /* DHCP 模式下获得IP */
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_eth_event_grp, ETH_CONNECTED_BIT);
    }
}

/* ------------------------------------------------------------------ */
esp_netif_t *get_eth_netif(void)
{
    return s_eth_netif;
}

esp_err_t eth_netif_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    return ESP_OK;
}

esp_err_t eth_start(IP4_info_t *net)
{
    if (net) {
        s_net_cfg = *net;
    } else {
        memset(&s_net_cfg, 0, sizeof(s_net_cfg));
        s_net_cfg.dhcp = 1;   /* 默认DHCP */
    }

    s_eth_event_grp = xEventGroupCreate();

    /* --- 手动复位 W5500 --- */
    gpio_config_t rst_cfg = {
        .pin_bit_mask = (1ULL << ETH_RST_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&rst_cfg);
    gpio_set_level(ETH_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(20));   /* RST 低电平保持 20ms */
    gpio_set_level(ETH_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(100));  /* 等待 W5500 内部 PLL 稳定 */

    /* --- INT 引脚配置上拉（W5500 INT 是开漏输出，必须上拉）--- */
    gpio_config_t int_cfg = {
        .pin_bit_mask = (1ULL << ETH_INT_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&int_cfg);

    /* --- SPI总线初始化 --- */
    spi_bus_config_t buscfg = {
        .miso_io_num   = ETH_SPI_MISO_GPIO,
        .mosi_io_num   = ETH_SPI_MOSI_GPIO,
        .sclk_io_num   = ETH_SPI_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(ETH_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    /* --- 原始SPI通信测试，读W5500版本寄存器 --- */
    w5500_spi_check();

    /* --- 安装GPIO中断服务（W5500 INT脚需要）--- */
    gpio_install_isr_service(0);

    /* --- W5500 SPI设备参数 --- */
    spi_device_interface_config_t devcfg = {
        .mode           = 0,
        .clock_speed_hz = ETH_SPI_CLOCK_MHZ * 1000 * 1000,
        .spics_io_num   = ETH_SPI_CS_GPIO,
        .queue_size     = 20,
    };

    /* --- W5500 MAC --- */
    eth_w5500_config_t w5500_cfg = ETH_W5500_DEFAULT_CONFIG(ETH_SPI_HOST, &devcfg);
    w5500_cfg.int_gpio_num = ETH_INT_GPIO;

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t   *mac     = esp_eth_mac_new_w5500(&w5500_cfg, &mac_cfg);

    /* --- W5500 PHY --- */
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.reset_gpio_num   = ETH_RST_GPIO;
    esp_eth_phy_t   *phy     = esp_eth_phy_new_w5500(&phy_cfg);

    /* --- 安装以太网驱动 --- */
    esp_eth_config_t  eth_cfg    = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t  eth_handle = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_cfg, &eth_handle));

    /* 从 eFuse 读取 MAC 地址并写入 W5500（必须在 driver_install 之后，因为 mac->init 会软复位清空 MAC）*/
    uint8_t mac_addr[6];
    esp_read_mac(mac_addr, ESP_MAC_ETH);
    mac->set_addr(mac, mac_addr);
    ESP_LOGI(TAG, "MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             mac_addr[0], mac_addr[1], mac_addr[2],
             mac_addr[3], mac_addr[4], mac_addr[5]);

    /* --- 创建以太网 netif --- */
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_cfg);

    /* --- 注册事件回调 --- */
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT,    ESP_EVENT_ANY_ID,     &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,     IP_EVENT_ETH_GOT_IP,  &eth_event_handler, NULL));

    /* --- 绑定netif到驱动并启动 --- */
    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(eth_handle);
    ESP_ERROR_CHECK(esp_netif_attach(s_eth_netif, glue));
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));

    /* --- 等待连接或超时（10秒）--- */
    EventBits_t bits = xEventGroupWaitBits(s_eth_event_grp,
                                           ETH_CONNECTED_BIT | ETH_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(10000));
    if (bits & ETH_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Ethernet ready");
    } else if (bits & ETH_FAIL_BIT) {
        ESP_LOGE(TAG, "Ethernet link failed");
    } else {
        ESP_LOGE(TAG, "Ethernet timeout");
    }

    return ESP_OK;
}

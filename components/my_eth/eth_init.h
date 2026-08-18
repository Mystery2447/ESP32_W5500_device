#ifndef ETH_INIT_H
#define ETH_INIT_H

#include "esp_err.h"
#include "esp_netif.h"
#include <stdint.h>

/* ---- W5500 SPI 引脚，根据实际接线修改 ---- */
#define ETH_SPI_HOST       SPI2_HOST   // HSPI
#define ETH_SPI_MISO_GPIO  27
#define ETH_SPI_MOSI_GPIO  26
#define ETH_SPI_SCLK_GPIO  14
#define ETH_SPI_CS_GPIO    12
#define ETH_INT_GPIO       25          // 支持内部上拉
#define ETH_RST_GPIO       33
#define ETH_SPI_CLOCK_MHZ  20          // 20MHz，W5500最高可达80MHz，但ESP32 SPI2/3在高于40MHz时可能不稳定
/* ----------------------------------------- */

typedef struct {
    char    ip[16];
    char    gateway[16];
    char    netmask[16];
    uint8_t dhcp;   /* 0 = static, 1 = DHCP */
} IP4_info_t;

esp_netif_t *get_eth_netif(void);
esp_err_t    eth_netif_init(void);
esp_err_t    eth_start(IP4_info_t *net);

#endif /* ETH_INIT_H */

#ifndef MY_NVS_H
#define MY_NVS_H

#include "esp_err.h"
#include "eth_init.h"   /* 提供 IP4_info_t */

void      init_nvs(void);

/* 网络配置（静态IP / DHCP）存取 */
esp_err_t get_network_info(IP4_info_t *network);
esp_err_t set_network_info(IP4_info_t tmp);
esp_err_t set_network_dhcp(IP4_info_t tmp);
esp_err_t read_network_info(IP4_info_t *network);

#endif /* MY_NVS_H */

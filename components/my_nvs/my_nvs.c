#include "my_nvs.h"
#include "eth_init.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_netif.h"
#include <string.h>

static const char *TAG = "MY_NVS";

void init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_LOGD(TAG, "flash erase...");
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS init success");
}

/* ---------- 网络配置（静态IP / DHCP）存取 ---------- */

esp_err_t set_network_dhcp(IP4_info_t tmp)
{
    nvs_handle_t handle;
    if (nvs_open("network_config", NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "could not open network_config NVS");
        return ESP_FAIL;
    }
    nvs_set_u8(handle, "dhcp", tmp.dhcp);
    nvs_commit(handle);
    nvs_close(handle);
    return ESP_OK;
}

esp_err_t set_network_info(IP4_info_t tmp)
{
    nvs_handle_t handle;
    esp_err_t err;
    if (nvs_open("network_config", NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "could not open network_config NVS");
        return ESP_FAIL;
    }
    err = nvs_set_str(handle, "ip",      tmp.ip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write ip failed");
        nvs_close(handle);
        return err;
    }
    nvs_set_str(handle, "gateway",  tmp.gateway);
    nvs_set_str(handle, "netmask",  tmp.netmask);
    nvs_set_u8 (handle, "dhcp",     tmp.dhcp);
    nvs_commit(handle);
    nvs_close(handle);
    return ESP_OK;
}

/* 仅读取NVS原始值，不访问netif */
esp_err_t read_network_info(IP4_info_t *network)
{
    nvs_handle_t handle;
    size_t num;
    if (network == NULL) return ESP_ERR_INVALID_ARG;

    nvs_open("network_config", NVS_READONLY, &handle);
    nvs_get_u8(handle, "dhcp", &network->dhcp);
    num = sizeof(network->ip);
    nvs_get_str(handle, "ip",      network->ip,      &num);
    num = sizeof(network->gateway);
    nvs_get_str(handle, "gateway", network->gateway, &num);
    num = sizeof(network->netmask);
    nvs_get_str(handle, "netmask", network->netmask, &num);
    nvs_close(handle);
    return ESP_OK;
}

/* 读取NVS配置；若DHCP模式则同时读取以太网netif当前IP */
esp_err_t get_network_info(IP4_info_t *network)
{
    nvs_handle_t handle;
    uint8_t flag_dhcp = 1;
    size_t  num       = 0;

    if (network == NULL) return ESP_ERR_INVALID_ARG;

    nvs_open("network_config", NVS_READONLY, &handle);
    esp_err_t err = nvs_get_u8(handle, "dhcp", &flag_dhcp);
    ESP_LOGD(TAG, "dhcp flag=%d", flag_dhcp);
    network->dhcp = flag_dhcp;

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "fail to open NVS: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    if (flag_dhcp == 0) {
        num = sizeof(network->ip);
        nvs_get_str(handle, "ip",      network->ip,      &num);
        num = sizeof(network->gateway);
        nvs_get_str(handle, "gateway", network->gateway, &num);
        num = sizeof(network->netmask);
        nvs_get_str(handle, "netmask", network->netmask, &num);
    } else {
        ESP_LOGI(TAG, "DHCP mode on");
        esp_netif_t *eth_if = get_eth_netif();
        if (eth_if == NULL) {
            nvs_close(handle);
            return ESP_FAIL;
        }
        esp_netif_ip_info_t ip_info;
        err = esp_netif_get_ip_info(eth_if, &ip_info);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "fail to read DHCP IP: %s", esp_err_to_name(err));
            nvs_close(handle);
            return err;
        }
        esp_ip4addr_ntoa(&ip_info.ip,      network->ip,      sizeof(network->ip));
        esp_ip4addr_ntoa(&ip_info.gw,      network->gateway, sizeof(network->gateway));
        esp_ip4addr_ntoa(&ip_info.netmask, network->netmask, sizeof(network->netmask));
        fprintf(stdout, "DHCP  IP:%s  GW:%s  Netmask:%s\n",
                network->ip, network->gateway, network->netmask);
    }

    nvs_close(handle);
    return ESP_OK;
}

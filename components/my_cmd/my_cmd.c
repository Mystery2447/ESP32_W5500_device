#include <stdbool.h>
#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "argtable3/argtable3.h"
#include <string.h>
#include "my_cmd.h"
#include "my_nvs.h"
#include "eth_init.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "relay.h"

#define CMD_UART_PORT   UART_NUM_1

static const char *TAG = "my_cmd";

/* ---- log_level 命令参数 ---- */
static struct {
    struct arg_str *tag;
    struct arg_str *level;
    struct arg_end *end;
} log_level_args;

/* ---- set_ipaddr 命令参数 ---- */
static struct {
    struct arg_str *ip_addr;
    struct arg_str *gateway;
    struct arg_str *netmask;
    struct arg_end *end;
} addr_args;

/* ---- set_dhcp 命令参数 ---- */
static struct {
    struct arg_str *dhcp;
    struct arg_end *end;
} dhcp_args;

static const char *s_log_level_names[] = {
    "none", "error", "warn", "info", "debug", "verbose"
};

/* ================================================================
 * version
 * ================================================================ */
static int get_version(int argc, char **argv)
{
    const char *model;
    esp_chip_info_t info;
    uint32_t flash_size;
    esp_chip_info(&info);

    switch (info.model) {
    case CHIP_ESP32:   model = "ESP32";    break;
    case CHIP_ESP32S2: model = "ESP32-S2"; break;
    case CHIP_ESP32S3: model = "ESP32-S3"; break;
    case CHIP_ESP32C3: model = "ESP32-C3"; break;
    case CHIP_ESP32H2: model = "ESP32-H2"; break;
    case CHIP_ESP32C2: model = "ESP32-C2"; break;
    default:           model = "Unknown";  break;
    }
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        printf("Get flash size failed\n");
        return 1;
    }
    printf("IDF Version:%s\r\n", esp_get_idf_version());
    printf("Chip info:\r\n");
    printf("\tmodel:%s\r\n", model);
    printf("\tcores:%d\r\n", info.cores);
    printf("\tfeature:%s%s%s%s%"PRIu32"%s\r\n",
           info.features & CHIP_FEATURE_WIFI_BGN ? "/802.11bgn" : "",
           info.features & CHIP_FEATURE_BLE      ? "/BLE"       : "",
           info.features & CHIP_FEATURE_BT       ? "/BT"        : "",
           info.features & CHIP_FEATURE_EMB_FLASH ? "/Embedded-Flash:" : "/External-Flash:",
           flash_size / (1024 * 1024), " MB");
    printf("\trevision number:%d\r\n", info.revision);
    return 0;
}

static void register_get_version(void)
{
    const esp_console_cmd_t cmd = {
        .command  = "version",
        .help     = "Get chip and IDF version information",
        .hint     = NULL,
        .func     = &get_version,
        .argtable = NULL
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/* ================================================================
 * log_level
 * ================================================================ */
static int log_level(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&log_level_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, log_level_args.end, argv[0]);
        return 1;
    }
    const char *tag_str   = log_level_args.tag->sval[0];
    const char *level_str = log_level_args.level->sval[0];
    esp_log_level_t level;
    size_t level_len = strlen(level_str);
    for (level = ESP_LOG_NONE; level <= ESP_LOG_VERBOSE; level++) {
        if (memcmp(level_str, s_log_level_names[level], level_len) == 0) break;
    }
    if (level > ESP_LOG_VERBOSE) {
        printf("Invalid log level '%s'\n", level_str);
        return 1;
    }
    if (level > CONFIG_LOG_MAXIMUM_LEVEL) {
        printf("Can't set log level to %s, max level is %s.\n",
               s_log_level_names[level], s_log_level_names[CONFIG_LOG_MAXIMUM_LEVEL]);
        return 1;
    }
    esp_log_level_set(tag_str, level);
    return 0;
}

static void register_log_level(void)
{
    log_level_args.tag   = arg_str1(NULL, NULL, "<tag|*>",                    "Log tag, or * for all");
    log_level_args.level = arg_str1(NULL, NULL, "<none|error|warn|debug|verbose>", "Log level");
    log_level_args.end   = arg_end(2);
    const esp_console_cmd_t cmd = {
        .command  = "log_level",
        .help     = "Set log level for a tag",
        .hint     = NULL,
        .func     = &log_level,
        .argtable = &log_level_args
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/* ================================================================
 * reset
 * ================================================================ */
static int reset_esp(int argc, char **argv)
{
    esp_restart();
    return 0;
}

static void register_reset(void)
{
    const esp_console_cmd_t cmd = {
        .command  = "reset",
        .help     = "Reset the ESP32",
        .func     = reset_esp,
        .argtable = NULL
    };
    esp_console_cmd_register(&cmd);
}

/* ================================================================
 * set_ipaddr  —  设置静态IP（保存到NVS，重启后生效）
 * ================================================================ */
static int set_static_ip4(int argc, char **argv)
{
    IP4_info_t static_ip = {0};
    int ret = arg_parse(argc, argv, (void **)&addr_args);
    if (ret != 0) {
        arg_print_errors(stderr, addr_args.end, argv[0]);
        return 1;
    }
    snprintf(static_ip.ip,      sizeof(static_ip.ip),      "%s", addr_args.ip_addr->sval[0]);
    snprintf(static_ip.gateway, sizeof(static_ip.gateway), "%s", addr_args.gateway->sval[0]);
    snprintf(static_ip.netmask, sizeof(static_ip.netmask), "%s", addr_args.netmask->sval[0]);
    static_ip.dhcp = 0;
    esp_err_t err = set_network_info(static_ip);
    if (err != ESP_OK) {
        fprintf(stderr, "Fail to set static IP\r\n");
        return 1;
    }
    fprintf(stdout, "Static IP saved. IP:%s  GW:%s  Netmask:%s\r\n",
            static_ip.ip, static_ip.gateway, static_ip.netmask);
    fprintf(stdout, "Please reset ESP to apply.\r\n");
    return 0;
}

static void register_set_ip4_cmd(void)
{
    addr_args.ip_addr  = arg_str1(NULL, NULL, "<ip>",      "Static IP address");
    addr_args.gateway  = arg_str1(NULL, NULL, "<gateway>", "Gateway address");
    addr_args.netmask  = arg_str1(NULL, NULL, "<netmask>", "Netmask");
    addr_args.end      = arg_end(3);
    const esp_console_cmd_t cmd = {
        .command  = "set_ipaddr",
        .help     = "Set static IPv4 address (saved to NVS)",
        .hint     = NULL,
        .func     = set_static_ip4,
        .argtable = &addr_args
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/* ================================================================
 * get_eth_ip4  —  读取以太网当前IP
 * ================================================================ */
static int get_eth_ip4_info(int argc, char **argv)
{
    esp_netif_t *eth_if = get_eth_netif();
    if (eth_if == NULL) {
        fprintf(stderr, "Ethernet netif not ready.\n");
        return 1;
    }
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(eth_if, &ip_info) != ESP_OK) {
        fprintf(stderr, "Get IP info failed.\n");
        return 1;
    }
    IP4_info_t info = {0};
    snprintf(info.ip,      sizeof(info.ip),      IPSTR, IP2STR(&ip_info.ip));
    snprintf(info.gateway, sizeof(info.gateway),  IPSTR, IP2STR(&ip_info.gw));
    snprintf(info.netmask, sizeof(info.netmask),  IPSTR, IP2STR(&ip_info.netmask));
    fprintf(stdout, "Ethernet IP: %s  GW: %s  Netmask: %s\n",
            info.ip, info.gateway, info.netmask);
    return 0;
}

static void register_get_ip4_cmd(void)
{
    const esp_console_cmd_t cmd = {
        .command  = "get_eth_ip4",
        .help     = "Get current Ethernet IPv4 info",
        .hint     = NULL,
        .func     = get_eth_ip4_info,
        .argtable = NULL
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/* ================================================================
 * set_dhcp
 * ================================================================ */
static int set_dhcp_cmd(int argc, char **argv)
{
    int ret = arg_parse(argc, argv, (void **)&dhcp_args);
    if (ret != 0) {
        arg_print_errors(stderr, dhcp_args.end, argv[0]);
        return 1;
    }
    IP4_info_t tmp = {0};
    if (strcmp(dhcp_args.dhcp->sval[0], "on") == 0) {
        tmp.dhcp = 1;
        set_network_dhcp(tmp);
        fprintf(stdout, "DHCP mode ON saved. Please reset ESP.\n");
    } else if (strcmp(dhcp_args.dhcp->sval[0], "off") == 0) {
        tmp.dhcp = 0;
        set_network_dhcp(tmp);
        fprintf(stdout, "DHCP mode OFF saved.\n");
    } else {
        fprintf(stderr, "Invalid arg: use on/off\n");
        return 1;
    }
    return 0;
}

static void register_set_dhcp_cmd(void)
{
    dhcp_args.dhcp = arg_str1(NULL, NULL, "<on|off>", "Enable or disable DHCP");
    dhcp_args.end  = arg_end(1);
    const esp_console_cmd_t cmd = {
        .command  = "set_dhcp",
        .help     = "Set DHCP on/off (saved to NVS)",
        .hint     = NULL,
        .func     = set_dhcp_cmd,
        .argtable = &dhcp_args
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/* ================================================================
 * read_nvs_network_info
 * ================================================================ */
static int read_network_info_cmd(int argc, char **argv)
{
    IP4_info_t network = {0};
    read_network_info(&network);
    fprintf(stdout, "NVS network: IP=%s  GW=%s  Netmask=%s  DHCP=%s\n",
            network.ip, network.gateway, network.netmask,
            network.dhcp ? "ON" : "OFF");
    return 0;
}

static void register_read_network_info_cmd(void)
{
    const esp_console_cmd_t cmd = {
        .command  = "read_nvs_network_info",
        .help     = "Read stored network info from NVS",
        .hint     = NULL,
        .func     = read_network_info_cmd,
        .argtable = NULL
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/* ================================================================
 * send_poweron  —  向 UART 发送 "poweron\r\n"
 * ================================================================ */
static int send_poweron_cmd(int argc, char **argv)
{
    const char *msg = "poweron\r\n";
    int len = uart_write_bytes(CMD_UART_PORT, msg, strlen(msg));
    if (len < 0) {
        fprintf(stderr, "uart write failed\n");
        return 1;
    }
    fprintf(stdout, "Sent: poweron\\r\\n (%d bytes)\n", len);
    return 0;
}

static void register_send_poweron(void)
{
    const esp_console_cmd_t cmd = {
        .command  = "send_poweron",
        .help     = "Send 'poweron\\r\\n' via UART",
        .hint     = NULL,
        .func     = send_poweron_cmd,
        .argtable = NULL
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/* ================================================================
 * relay  —  控制继电器 GPIO
 *   用法: relay <1|2> <on|off>
 * ================================================================ */
static struct {
    struct arg_int *num;
    struct arg_str *state;
    struct arg_end *end;
} relay_args;

static int relay_cmd(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&relay_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, relay_args.end, argv[0]);
        return 1;
    }
    int num = relay_args.num->ival[0];
    if (num < 1 || num > 2) {
        fprintf(stderr, "Invalid relay number: %d (use 1 or 2)\n", num);
        return 1;
    }
    const char *state = relay_args.state->sval[0];
    if (strcmp(state, "on") == 0) {
        relay_set(num, true);
        fprintf(stdout, "Relay%d ON\n", num);
    } else if (strcmp(state, "off") == 0) {
        relay_set(num, false);
        fprintf(stdout, "Relay%d OFF\n", num);
    } else {
        fprintf(stderr, "Invalid arg: use on/off\n");
        return 1;
    }
    return 0;
}

static void register_relay_cmd(void)
{
    relay_init();   /* 初始化两路继电器，默认关闭 */

    relay_args.num   = arg_int1(NULL, NULL, "<1|2>", "Relay number (1 or 2)");
    relay_args.state = arg_str1(NULL, NULL, "<on|off>", "Relay state");
    relay_args.end   = arg_end(2);
    const esp_console_cmd_t cmd = {
        .command  = "relay",
        .help     = "Control relay (relay <1|2> <on|off>)",
        .hint     = NULL,
        .func     = relay_cmd,
        .argtable = &relay_args
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/* ================================================================
 * 注册所有命令
 * ================================================================ */
void register_my_cmd(void)
{
    ESP_LOGI(TAG, "Registering custom commands");
    register_reset();
    register_log_level();
    register_get_version();
    register_get_ip4_cmd();
    register_set_ip4_cmd();
    register_set_dhcp_cmd();
    register_read_network_info_cmd();
    register_send_poweron();
    register_relay_cmd();
}

# ESP32 以太网串口服务器 使用文档

## 一、硬件连接

### 1.1 串口接线

| 功能 | ESP32 引脚 | 方向 | 说明 |
|------|-----------|------|------|
| MCU UART TX | GPIO18 | ESP32 → MCU | 接 MCU 设备 RX |
| MCU UART RX | GPIO19 | MCU → ESP32 | 接 MCU 设备 TX |
| SOC UART TX | GPIO17 | ESP32 → SOC | 接 SOC 设备 RX |
| SOC UART RX | GPIO5 | SOC → ESP32 | 接 SOC 设备 TX |
| 调试串口 TX | GPIO1 | ESP32 → PC | 本地控制台 |
| 调试串口 RX | GPIO3 | PC → ESP32 | 本地控制台 |

### 1.2 以太网接线

| 功能 | ESP32 引脚 | 说明 |
|------|-----------|------|
| SPI MISO | GPIO27 | W5500 MISO |
| SPI MOSI | GPIO26 | W5500 MOSI |
| SPI SCLK | GPIO14 | W5500 SCLK |
| SPI CS | GPIO12 | W5500 SCSn |
| INT | GPIO25 | W5500 INTn（内部上拉） |
| RST | GPIO33 | W5500 RSTn |

### 1.3 继电器接线

| 功能 | ESP32 引脚 | 说明 |
|------|-----------|------|
| 继电器1 | GPIO15 | 高电平吸合 |
| 继电器2 | GPIO13 | 高电平吸合 |

---

## 二、网络配置

### 2.1 首次使用（DHCP 模式）

设备默认使用 DHCP 自动获取 IP。上电后通过本地调试串口（115200bps）查看日志：

```
ETH_INIT: Ethernet ready
ETH_INIT: Got IP: 192.168.1.100
```

### 2.2 设置静态 IP

通过本地控制台设置静态 IP（重启后生效）：

```
esp32_zfc> set_ipaddr 192.168.1.200 192.168.1.1 255.255.255.0
Static IP saved. IP:192.168.1.200  GW:192.168.1.1  Netmask:255.255.255.0
Please reset ESP to apply.
esp32_zfc> reset
```

### 2.3 切换回 DHCP

```
esp32_zfc> set_dhcp on
DHCP mode ON saved. Please reset ESP.
esp32_zfc> reset
```

### 2.4 查看当前配置

```
esp32_zfc> get_eth_ip4           # 查看当前以太网IP
esp32_zfc> read_nvs_network_info  # 查看NVS中存储的网络配置
```

---

## 三、客户端连接

### 3.1 使用 Telnet 连接

```bash
telnet 192.168.1.100 8080
```

### 3.2 使用 netcat 连接

```bash
nc 192.168.1.100 8080
```

### 3.3 使用 PuTTY

- Connection type: **Telnet**（推荐）或 **Raw**
- Host Name: `192.168.1.100`
- Port: `8080`

连接成功后显示：

```
welcome to esp32 tcp server
type 'exit()' to disconnect
type 'relay <1|2> <on|off>' to control relay
type 'read_mcu_uart' to read mcu uart data
type 'read_soc_uart' to read soc uart data
```

---

## 四、远程命令

### 4.1 继电器控制

```
relay 1 on      # 继电器1 吸合
relay 1 off     # 继电器1 断开
relay 2 on      # 继电器2 吸合
relay 2 off     # 继电器2 断开
```

### 4.2 进入 MCU UART 透传模式

```
read_mcu_uart
```

- 进入后所有键盘输入将通过 UART1 发送给 MCU 设备
- 以行为单位发送（按回车键发送整行）
- 输入 `exit()` 退出透传模式

### 4.3 进入 SOC UART 透传模式

```
read_soc_uart
```

- 进入后所有键盘输入将逐字通过 UART2 发送给 SOC 设备
- 字符模式，每个按键即时发送
- 输入 `exit()` 退出透传模式

### 4.4 断开连接

```
exit()
```

---

## 五、本地控制台命令

通过调试串口（UART0，115200bps）连接 ESP32，输入 `help` 查看所有命令：

| 命令 | 参数 | 说明 |
|------|------|------|
| `help` | - | 查看所有命令 |
| `version` | - | 查看芯片型号和 IDF 版本 |
| `log_level` | `<tag\|*> <none\|error\|warn\|debug\|verbose>` | 设置日志级别 |
| `reset` | - | 重启 ESP32 |
| `set_ipaddr` | `<ip> <gateway> <netmask>` | 设置静态 IP |
| `set_dhcp` | `<on\|off>` | 切换 DHCP |
| `get_eth_ip4` | - | 查看当前以太网 IP |
| `read_nvs_network_info` | - | 查看 NVS 网络配置 |
| `send_poweron` | - | 通过 UART1 发送 poweron 指令 |
| `relay` | `<1\|2> <on\|off>` | 控制继电器 |

---

## 六、工作模式切换

```
┌──────────┐  read_mcu_uart   ┌───────────────┐  exit()  ┌──────────┐
│          │ ────────────────▶ │ MCU UART 透传 │ ───────▶ │          │
│  命令模式 │                   └───────────────┘          │  命令模式 │
│          │  read_soc_uart   ┌───────────────┐  exit()  │          │
│          │ ────────────────▶ │ SOC UART 透传 │ ───────▶ │          │
└──────────┘                   └───────────────┘          └──────────┘
```

**注意**：进入 SOC UART 透传模式时会自动退出 MCU UART 透传模式。

---

## 七、并发限制

| 限制项 | 最大值 | 说明 |
|--------|--------|------|
| 同时连接客户端 | 3 | 超出后新连接将等待 |
| MCU UART 独占 | 1 个客户端 | 其他客户端需等待 |
| SOC UART 独占 | 1 个客户端 | 其他客户端需等待 |

---

## 八、常见问题

### 无法连接

1. 确认 ESP32 和 PC 在同一网络
2. 确认 IP 地址正确（通过本地串口 `get_eth_ip4` 查看）
3. 确认端口为 8080

### 串口透传无响应

1. 确认目标设备已正确连接 UART 引脚
2. 确认波特率匹配（默认 115200bps）
3. 确认目标设备有回显

### 继电器不工作

1. 确认 GPIO 引脚正确
2. 确认继电器模块电平匹配（ESP32 为 3.3V 电平）

### Telnet 客户端显示异常

推荐使用支持 Telnet 协议的客户端（如 PuTTY Telnet 模式），ESP32 会自动处理 Telnet 协商。如果使用 Raw TCP 模式（如 netcat），也能正常工作，但不会有 Telnet 字符模式协商。
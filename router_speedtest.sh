#!/bin/bash

# ============================================================
# router_speedtest.sh — 路由器网速测试脚本
# 用法: ./router_speedtest.sh [internet|local|all]
# ============================================================

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

# ---------- 配置 ----------
ROUTER_IP="192.168.8.1"          # 路由器地址
PING_COUNT=20                     # ping 包数
IPERF_PORT=5201

usage() {
    echo "用法: $0 [internet|local|all]"
    echo "  internet  — 测试外网速度 (speedtest-cli)"
    echo "  local     — 测试内网延迟和丢包"
    echo "  all       — 全部测试（默认）"
    exit 1
}

check_cmd() {
    if ! command -v "$1" &>/dev/null; then
        echo -e "${RED}[错误] 缺少 $1，请先安装: sudo apt install $2${NC}"
        return 1
    fi
}

# ---------- 外网测速 ----------
test_internet() {
    echo -e "${GREEN}========== 外网测速 ==========${NC}"

    if check_cmd speedtest-cli speedtest-cli; then
        speedtest-cli --simple
    elif check_cmd speedtest speedtest-cli; then
        speedtest --simple
    else
        # 备选：curl 下载测试
        local url="http://speedtest.tele2.net/10MB.zip"
        echo -e "${YELLOW}[备选] 下载 10MB 文件测速...${NC}"
        echo "  下载地址: $url"
        curl -s -o /dev/null -w "  下载速度: %{speed_download} bytes/s\n  耗时: %{time_total}s\n" "$url"
    fi
}

# ---------- 内网测速 ----------
test_local() {
    echo -e "${GREEN}========== 内网测试 (目标: $ROUTER_IP) ==========${NC}"

    # 1. Ping 延迟和丢包
    echo -e "\n${YELLOW}--- Ping 延迟与丢包 ($PING_COUNT 包) ---${NC}"
    ping -c "$PING_COUNT" -q "$ROUTER_IP" | tail -2

    # 2. ARP 延迟（同网段第一跳）
    echo -e "\n${YELLOW}--- ARP 延迟 ---${NC}"
    arping -c 5 -I eth0 "$ROUTER_IP" 2>/dev/null || echo "  跳过（需要 arping）"

    # 3. iperf3 局域网吞吐量
    if check_cmd iperf3 iperf3; then
        echo -e "\n${YELLOW}--- iperf3 上传测试 (本机 → 路由器) ---${NC}"
        if iperf3 -c "$ROUTER_IP" -p "$IPERF_PORT" -t 10 2>/dev/null; then
            echo -e "\n${GREEN}iperf3 测试完成${NC}"
        else
            echo -e "${YELLOW}[提示] 路由器未开启 iperf3 服务${NC}"
            echo "  在路由器上运行: iperf3 -s"
        fi
    fi
}

# ---------- 主流程 ----------
MODE="${1:-all}"

echo -e "${GREEN}"
echo "  ╔══════════════════════════════╗"
echo "  ║    路由器网速测试工具        ║"
echo "  ╚══════════════════════════════╝"
echo -e "${NC}"

case "$MODE" in
    internet) test_internet ;;
    local)    test_local ;;
    all)
        test_internet
        echo ""
        test_local
        ;;
    *) usage ;;
esac

echo -e "\n${GREEN}测试完成.${NC}"
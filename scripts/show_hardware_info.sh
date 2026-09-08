#!/bin/bash

echo "GitHub Runner 硬件信息概览"
echo "=========================="

# CPU信息
echo -e "\n\033[1;34mCPU信息:\033[0m"
cpu_model=$(grep "model name" /proc/cpuinfo | head -1 | cut -d: -f2 | sed 's/^ *//')
cpu_cores=$(grep -c "^processor" /proc/cpuinfo)
cpu_threads=$(lscpu 2>/dev/null | grep "CPU(s):" | head -1 | awk '{print $2}' || echo "$cpu_cores")
echo "型号: $cpu_model"
echo "核心/线程: $cpu_cores/$cpu_threads"

# 内存信息
echo -e "\n\033[1;34m内存信息:\033[0m"
mem_total=$(free -h | grep Mem: | awk '{print $2}')
mem_used=$(free -h | grep Mem: | awk '{print $3}')
swap_total=$(free -h | grep Swap: | awk '{print $2}')
echo "物理内存: $mem_total (已用: $mem_used)"
echo "虚拟内存: $swap_total"

# 存储信息
echo -e "\n\033[1;34m存储信息:\033[0m"
disk_total=$(df -h / | awk 'NR==2 {print $2}')
disk_used=$(df -h / | awk 'NR==2 {print $3}')
disk_free=$(df -h / | awk 'NR==2 {print $4}')
echo "根分区: 总$disk_total, 已用$disk_used, 剩余$disk_free"

# 系统信息
echo -e "\n\033[1;34m系统信息:\033[0m"
os_info=$(lsb_release -d 2>/dev/null | cut -f2 || echo "Ubuntu (具体版本未知)")
kernel_version=$(uname -r)
arch=$(uname -m)
echo "操作系统: $os_info"
echo "内核版本: $kernel_version"
echo "架构: $arch"

# GitHub Runner信息
echo -e "\n\033[1;34mRunner信息:\033[0m"
if [ -n "${RUNNER_NAME}" ]; then
    echo "Runner名称: $RUNNER_NAME"
fi
if [ -n "${GITHUB_ACTIONS}" ]; then
    echo "环境: GitHub Actions"
fi

# 性能测试（可选）
echo -e "\n\033[1;34m简单性能测试:\033[0m"
start_time=$(date +%s.%N)
for i in {1..1000}; do : ; done
end_time=$(date +%s.%N)
echo "空循环测试: $(echo "$end_time - $start_time" | bc) 秒"

echo -e "\n=========================="
echo "信息收集完成"

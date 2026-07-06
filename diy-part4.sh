#!/bin/bash

# Modify default IP
sed -i 's/192.168.1.1/192.168.100.1/g' package/base-files/files/bin/config_generate

# turboacc
curl -sSL https://raw.githubusercontent.com/mufeng05/turboacc/main/add_turboacc.sh -o add_turboacc.sh && bash add_turboacc.sh

# temp
git clone https://github.com/gSpotx2f/luci-app-temp-status package/luci-app-temp-status
git clone https://github.com/gSpotx2f/luci-app-cpu-perf package/luci-app-cpu-perf

# OpenClash
git clone --depth 1 https://github.com/vernesong/OpenClash.git OpenClash

# luci-app-airoha-npu
if [ ! -d "package/luci-app-airoha-npu" ]; then
    git clone https://github.com/rchen14b/luci-app-airoha-npu.git package/luci-app-airoha-npu
fi


echo "src-git airoha-npu https://github.com/rchen14b/luci-app-airoha-npu" >> openwrt/feeds.conf.default


# 验证
echo "=== 验证 airoha-npu ==="
ls -l package/luci-app-airoha-npu
ls -l package/luci-app-airoha-npu/Makefile

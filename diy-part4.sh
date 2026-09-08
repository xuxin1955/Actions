#!/bin/bash

# Modify default IP
sed -i 's/192.168.1.1/192.168.10.1/g' package/base-files/files/bin/config_generate

# turboacc
# curl -sSL https://raw.githubusercontent.com/mufeng05/turboacc/main/add_turboacc.sh -o add_turboacc.sh && bash add_turboacc.sh
curl -sSL https://raw.githubusercontent.com/chenmozhijin/turboacc/luci/add_turboacc.sh -o add_turboacc.sh && bash add_turboacc.sh

# temp
git clone https://github.com/gSpotx2f/luci-app-temp-status package/luci-app-temp-status
git clone https://github.com/gSpotx2f/luci-app-cpu-perf package/luci-app-cpu-perf

# OpenClash
git clone --depth 1 https://github.com/vernesong/OpenClash.git OpenClash

# QModem
echo "src-git qmodem https://github.com/FUjr/QModem.git;main" >> "$GITHUB_WORKSPACE/openwrt/feeds.conf.default"

# luci-app-airoha-npu
if [ ! -d "package/luci-app-airoha-npu" ]; then
    git clone https://github.com/bingoguo93/luci-app-airoha-npu package/luci-app-airoha-npu
fi

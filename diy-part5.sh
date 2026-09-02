#!/bin/bash

# Modify default IP
sed -i 's/192.168.1.1/192.168.10.1/g' package/base-files/files/bin/config_generate

# turboacc
# curl -sSL https://raw.githubusercontent.com/mufeng05/turboacc/main/add_turboacc.sh -o add_turboacc.sh && bash add_turboacc.sh
# curl -sSL https://raw.githubusercontent.com/chenmozhijin/turboacc/luci/add_turboacc.sh -o add_turboacc.sh && bash add_turboacc.sh

# QModem
echo "src-git qmodem https://github.com/FUjr/QModem.git;main" >> "$GITHUB_WORKSPACE/openwrt/feeds.conf.default"

# hlk7620A dts
mkdir -p target/linux/ramips/dts/
# cp -f "$GITHUB_WORKSPACE/scripts/mt7620/mt7620a_glinet_gl-mt750.dts" "target/linux/ramips/dts/mt7620a_glinet_gl-mt750.dts"
# cp -f "$GITHUB_WORKSPACE/scripts/mt7620/mt7620a_xiaomi_miwifi-mini.dts" "target/linux/ramips/dts/mt7620a_xiaomi_miwifi-mini.dts"
cp -f "$GITHUB_WORKSPACE/scripts/mt7620a_xiaomi_miwifi-mini.dts" "target/linux/ramips/dts/mt7620a_xiaomi_miwifi-mini.dts"



# mkdir -p "target/linux/ramips/mt7620/base-files/etc/board.d/"
# cp -f "$GITHUB_WORKSPACE/scripts/mt7620/01_leds" "target/linux/ramips/mt7620/base-files/etc/board.d/01_leds"

# 北大源
# cp -r "$GITHUB_WORKSPACE/scripts/files-7620" "$GITHUB_WORKSPACE/openwrt/files"
# ls -R "$GITHUB_WORKSPACE/openwrt/files"


sed -i 's/^root:[^:]*:/root::/' package/base-files/files/etc/shadow
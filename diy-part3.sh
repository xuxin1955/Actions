#!/bin/bash
#

# Modify default IP
sed -i 's/192.168.1.1/192.168.10.1/g' package/base-files/files/bin/config_generate

# hlk7628n dts
mkdir -p target/linux/ramips/dts/
cp -f "$GITHUB_WORKSPACE/scripts/dts/mt7628an_hilink_hlk-7628n.dts" "target/linux/ramips/dts/mt7628an_hilink_hlk-7628n.dts"

# hlk7628n (LEDs)
mkdir -p "target/linux/ramips/mt76x8/base-files/etc/board.d/"
cp -f "$GITHUB_WORKSPACE/scripts/image/01_leds" "target/linux/ramips/mt76x8/base-files/etc/board.d/01_leds"

# hlk7628n (LEDs)
mkdir -p package/kernel/mt76/
cp -f "$GITHUB_WORKSPACE/scripts/image/Makefile" "package/kernel/mt76/Makefile"

# 北大源
cp -r "$GITHUB_WORKSPACE/scripts/files-7628" "$GITHUB_WORKSPACE/openwrt/files"
ls -R "$GITHUB_WORKSPACE/openwrt/files"

# QModem
echo "src-git qmodem https://github.com/FUjr/QModem.git;main" >> "$GITHUB_WORKSPACE/openwrt/feeds.conf.default"

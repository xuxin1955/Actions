#!/bin/bash

# Modify default IP
sed -i 's/192.168.1.1/192.168.10.1/g' package/base-files/files/bin/config_generate

# hlk7628dna dts
mkdir -p target/linux/ramips/dts/
cp -f "$GITHUB_WORKSPACE/scripts/dts/mt7628an_hilink_hlk-7628d.dts" "target/linux/ramips/dts/mt7628an_hilink_hlk-7628d.dts"

# hlk7628dna mk
mkdir -p target/linux/ramips/image/
cp -f "$GITHUB_WORKSPACE/scripts/image/mt76x8.mk" "target/linux/ramips/image/mt76x8.mk"

# hlk7628dna board.d
mkdir -p "target/linux/ramips/mt76x8/base-files/etc/board.d/"
cp -f "$GITHUB_WORKSPACE/scripts/image/02_network" "target/linux/ramips/mt76x8/base-files/etc/board.d/02_network"


# hlk7628dna (LEDs配置)
mkdir -p "target/linux/ramips/mt76x8/base-files/etc/board.d/"
cp -f "$GITHUB_WORKSPACE/scripts/image/01_leds" "target/linux/ramips/mt76x8/base-files/etc/board.d/01_leds"



# 北大源
cp -r "$GITHUB_WORKSPACE/scripts/files-7628" "$GITHUB_WORKSPACE/openwrt/files"
ls -R "$GITHUB_WORKSPACE/openwrt/files"

# QModem
echo "src-git qmodem https://github.com/FUjr/QModem.git;main" >> "$GITHUB_WORKSPACE/openwrt/feeds.conf.default"

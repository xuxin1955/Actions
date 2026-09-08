#!/bin/bash

# Modify default IP
sed -i 's/192.168.1.1/192.168.10.1/g' package/base-files/files/bin/config_generate


# hlk7621 dts
# mkdir -p target/linux/ramips/dts/
# cp -f "$GITHUB_WORKSPACE/scripts/mt7621/mt7621_hilink_hlk-7621a-evb.dts" "target/linux/ramips/dts/mt7621_hilink_hlk-7621a-evb.dts"

# hlk7621 dts
# mkdir -p target/linux/ramips/dts/
# cp -f "$GITHUB_WORKSPACE/scripts/mt7621/mt7621_hilink_hlk-7621a.dts" "target/linux/ramips/dts/mt7621_hilink_hlk-7621a.dts"


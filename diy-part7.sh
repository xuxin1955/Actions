#!/bin/bash

# Modify default IP
sed -i 's/192.168.1.1/192.168.10.1/g' package/base-files/files/bin/config_generate


# hlk7620A dts
mkdir -p target/linux/ramips/dts/
cp -f "$GITHUB_WORKSPACE/scripts/mt7620/mt7620a_glinet_gl-mt750.dts" "target/linux/ramips/dts/mt7620a_glinet_gl-mt750.dts"

mkdir -p "target/linux/ramips/mt7620/base-files/etc/board.d/"
cp -f "$GITHUB_WORKSPACE/scripts/mt7620/01_leds" "target/linux/ramips/mt7620/base-files/etc/board.d/01_leds"


#2. Clear the login password
# sed -i 's/$1$V4UetPzk$CYXluq4wUazHjmCDBCqXF.//g' package/lean/default-settings/files/zzz-default-settings

#!/bin/bash

# 8916dts
mkdir -p target/linux/msm89xx/dts/
cp -f "$GITHUB_WORKSPACE/scripts/dts/msm8916.dtsi" "target/linux/msm89xx/dts/msm8916.dtsi"

# OpenClash
# git clone --depth 1 https://github.com/vernesong/OpenClash.git OpenClash

# OpenAppFilter
git clone --depth 1 https://github.com/destan19/OpenAppFilter.git package/OpenAppFilter

# turboacc
# curl -sSL https://raw.githubusercontent.com/mufeng05/turboacc/main/add_turboacc.sh -o add_turboacc.sh && bash add_turboacc.sh
curl -sSL https://raw.githubusercontent.com/chenmozhijin/turboacc/luci/add_turboacc.sh -o add_turboacc.sh && bash add_turboacc.sh

# 调试
sed -i 's|src-git-full openstick https://github.com/lkiuyu/openstick-feeds.git|src-git-full openstick https://github.com/xuxin1955/openstick-feeds|g' feeds.conf.default


# inspect
echo ">>> OPP VOLTAGE COUNT: $(grep -c 'opp-microvolt' target/linux/msm89xx/dts/msm8916.dtsi) <<<"


# kenzo
echo 'src-git kenzo https://github.com/kenzok8/openwrt-packages' >> feeds.conf.default

# small
echo 'src-git small https://github.com/kenzok8/small' >> feeds.conf.default


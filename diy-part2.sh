#!/bin/bash
#

# Modify default IP
sed -i 's/192.168.1.1/192.168.10.1/g' package/base-files/files/bin/config_generate

# eth0
sed -i "s/ucidef_set_interface_lan 'eth0'/ucidef_set_interface_lan 'br-lan'/" package/base-files/files/etc/board.d/99-default_network

# Modify default theme
sed -i 's/luci-theme-material/luci-theme-argon/g' feeds/luci/collections/luci/Makefile


# 北大源
cp -r "$GITHUB_WORKSPACE/scripts/files-8916" "$GITHUB_WORKSPACE/openwrt/files"


# 压制 GCC 14 类型错误 / dtc 参数（独立补丁）
[ -f include/toplevel.mk ] || cd "$GITHUB_WORKSPACE/openwrt" || exit 1
patch -p1 --forward < "$GITHUB_WORKSPACE/scripts/msm89xx/patches/001-libnftnl-no-error-incompatible-pointer-types.patch" || {
	echo "::error::001-libnftnl-no-error-incompatible-pointer-types.patch failed to apply: upstream libnftnl Makefile has drifted; re-sync scripts/msm89xx/patches/001-libnftnl-no-error-incompatible-pointer-types.patch"
	exit 1
}

patch -p1 --forward < "$GITHUB_WORKSPACE/scripts/msm89xx/patches/002-image-mk-remove-graph-child-address.patch" || {
	echo "::error::002-image-mk-remove-graph-child-address.patch failed to apply: upstream image.mk has drifted; re-sync scripts/msm89xx/patches/002-image-mk-remove-graph-child-address.patch"
	exit 1
}

#!/bin/bash

# msm8916
cp -rf "$GITHUB_WORKSPACE/scripts/msm89xx/target/." target/
cp -rf "$GITHUB_WORKSPACE/scripts/msm89xx/package/." package/
cp -rf "$GITHUB_WORKSPACE/scripts/msm89xx/toolchain/." toolchain/
cp -rf "$GITHUB_WORKSPACE/scripts/msm89xx/feeds.conf.default" feeds.conf.default
# 验证
ls target/linux/msm89xx/Makefile package/kernel/mac80211/patches/ath/*wcn36xx* package/kernel/mac80211/ath.mk toolchain/musl/include/sys/glibc-types.h

# OpenAppFilter
git clone --depth 1 https://github.com/destan19/luci-app-harbor-file.git package/harbor-file

# turboacc
# curl -sSL https://raw.githubusercontent.com/mufeng05/turboacc/main/add_turboacc.sh -o add_turboacc.sh && bash add_turboacc.sh
# curl -sSL https://raw.githubusercontent.com/chenmozhijin/turboacc/luci/add_turboacc.sh -o add_turboacc.sh && bash add_turboacc.sh

# kenzo
echo 'src-git kenzo https://github.com/kenzok8/openwrt-packages' >> feeds.conf.default

# small
echo 'src-git small https://github.com/kenzok8/small' >> feeds.conf.default

# kernel-7.3
(
set -u
IDX=https://cdn.kernel.org/pub/linux/kernel/v7.x/
KVF=target/linux/generic/kernel-7.3
DEF_SUB=3
DEF_HASH=79c66dde66cd96877fcde8f7d36aec946b1f423d2b97b5a7b1751b75c764f3d1
SUB=$DEF_SUB
HASH=$DEF_HASH
LATEST=0
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
echo "===== 上游 v7.x 现有内核包 ====="
if curl -fsS --max-time 60 "$IDX" -o "$W/i" && curl -fsS --max-time 60 "${IDX}sha256sums.asc" -o "$W/s"; then
  grep -oE 'linux-7\.[0-9]+(\.[0-9]+)?\.tar\.xz' "$W/i" | sort -V | uniq
  N=$(grep -oE 'linux-7\.3\.[0-9]+\.tar\.xz' "$W/i" | sed -E 's/^linux-7\.3\.//; s/\.tar\.xz$//' | sort -n | tail -1)
  case "$N" in ''|*[!0-9]*) N=$DEF_SUB ;; esac
  if [ "$N" -ge "$DEF_SUB" ]; then
    H=$(awk -v f="linux-7.3.$N.tar.xz" '$2==f{print $1}' "$W/s")
    if printf '%s' "$H" | grep -qE '^[0-9a-f]{64}$'; then
      SUB=$N
      HASH=$H
      LATEST=1
    fi
  fi
else
  echo "(CDN index or sha256sums.asc unavailable)"
fi
printf 'LINUX_VERSION-7.3 = .%s\nLINUX_KERNEL_HASH-7.3.%s = %s\n' "$SUB" "$SUB" "$HASH" > "$KVF"
if [ "$LATEST" = 1 ]; then
  echo "kernel-7.3 -> 7.3.$SUB (auto latest)"
else
  echo "::warning::kernel-7.3 -> 7.3.$SUB (fallback: CDN unreachable or hash missing)"
fi
cat "$KVF"
)

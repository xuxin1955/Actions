#!/bin/bash

cp -rf "$GITHUB_WORKSPACE/scripts/sy108/target/." target/
cp -rf "$GITHUB_WORKSPACE/scripts/sy108/include/." include/
cp -rf "$GITHUB_WORKSPACE/scripts/sy108/package/." package/

patch -p1 --forward < "$GITHUB_WORKSPACE/scripts/sy108/patches/050-dnsmasq-jail-pidfile.patch" || {
	echo "::error::050-dnsmasq-jail-pidfile.patch failed to apply: upstream dnsmasq.init has drifted; re-sync scripts/sy108/patches/050-dnsmasq-jail-pidfile.patch"
	exit 1
}

patch -p1 --forward < "$GITHUB_WORKSPACE/scripts/sy108/patches/040-sysupgrade-prepare-hook.patch" || {
	echo "::error::040-sysupgrade-prepare-hook.patch failed to apply: upstream sysupgrade has drifted; re-sync scripts/sy108/patches/040-sysupgrade-prepare-hook.patch"
	exit 1
}

chmod 755 package/base-files/files/sbin/sysupgrade

ls target/linux/sprd/Makefile \
   target/linux/generic/kernel-7.3 \
   include/kernel.mk \
   package/sy108-image/Makefile \
   package/sy108-image/src/tools/image/capsule.py

f=package/base-files/files/sbin/sysupgrade
grep -q 'platform_prepare_upgrade' "$f" && [ -x "$f" ] && echo "sysupgrade prepare hook OK" || {
	echo "::error::sysupgrade prepare hook missing or not executable after patching"
	exit 1
}
# kernel-7.3 自动跟随上游最新
(
set -u
IDX=https://cdn.kernel.org/pub/linux/kernel/v7.x/
KVF=target/linux/generic/kernel-7.3
DEF_SUB=6
DEF_HASH=8183d475a76a0faffa92d6cd500af421f589dfd0300a0f1060bb875e11d45723
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

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

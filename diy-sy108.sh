#!/bin/bash

cp -rf "$GITHUB_WORKSPACE/scripts/sy108/target/." target/
cp -rf "$GITHUB_WORKSPACE/scripts/sy108/include/." include/
cp -rf "$GITHUB_WORKSPACE/scripts/sy108/package/." package/

chmod 755 target/linux/sprd/base-files/sbin/sysupgrade

ls target/linux/sprd/Makefile \
   target/linux/generic/kernel-7.3 \
   include/kernel.mk \
   package/sy108-image/Makefile \
   package/sy108-image/src/tools/image/capsule.py

grep -q 'platform_prepare_upgrade' target/linux/sprd/base-files/sbin/sysupgrade && echo "sysupgrade hook OK"
[ -x target/linux/sprd/base-files/sbin/sysupgrade ] && echo "sysupgrade executable OK"

#!/bin/bash

cd $GITHUB_WORKSPACE/openwrt || exit 1

[ -f feeds/packages/net/onionshare-cli/Makefile ] || exit 0

sed -i '/+python3-unidecode/d' feeds/packages/net/onionshare-cli/Makefile
echo "✅ python3-unidecode removed"

sed -i '/+python3-pysocks/d' feeds/packages/net/onionshare-cli/Makefile
echo "✅ python3-pysocks removed"

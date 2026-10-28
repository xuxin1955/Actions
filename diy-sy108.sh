#!/bin/bash
set -e

overlay="$GITHUB_WORKSPACE/scripts/sy108/overlay"
root="$GITHUB_WORKSPACE/sy108"

[ -d "$overlay" ] || exit 0

if [ -d "$overlay/packages" ]; then
	cp -a "$overlay/packages/." "$root/packages/"
fi

if [ -d "$overlay/integration/openwrt/target/linux/sprd" ]; then
	cp -a "$overlay/integration/openwrt/target/linux/sprd/." ./target/linux/sprd/
fi

for frag in "$overlay"/configs/*.append; do
	[ -f "$frag" ] || continue
	target="$root/configs/$(basename "$frag" .append)"
	[ -f "$target" ] || continue
	while IFS= read -r line; do
		[ -n "$line" ] || continue
		grep -qxF "$line" "$target" || printf '%s\n' "$line" >> "$target"
	done < "$frag"
done

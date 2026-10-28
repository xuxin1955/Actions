#!/bin/bash
set -e

overlay="$GITHUB_WORKSPACE/scripts/sy108/overlay"
root="$GITHUB_WORKSPACE/sy108"

[ -d "$overlay" ] || exit 0

for dir in "$overlay"/*/; do
	[ -d "$dir" ] || continue
	name=$(basename "$dir")
	[ "$name" = configs ] && continue
	[ -d "$root/$name" ] || continue
	cp -a "$dir." "$root/$name/"
done

for frag in "$overlay"/configs/*.append; do
	[ -f "$frag" ] || continue
	target="$root/configs/$(basename "$frag" .append)"
	[ -f "$target" ] || continue
	while IFS= read -r line; do
		[ -n "$line" ] || continue
		grep -qxF "$line" "$target" || printf '%s\n' "$line" >> "$target"
	done < "$frag"
done

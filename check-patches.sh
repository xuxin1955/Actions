#!/usr/bin/env bash
set -u

OPENWRT_DIR="${1:-}"
if [ -z "$OPENWRT_DIR" ]; then
	for d in openwrt .; do
		if [ -f "$d/package/kernel/mac80211/Makefile" ]; then
			OPENWRT_DIR="$d"
			break
		fi
	done
fi
if [ -z "$OPENWRT_DIR" ] || [ ! -f "$OPENWRT_DIR/package/kernel/mac80211/Makefile" ]; then
	echo "::error::openwrt tree not found" >&2
	exit 2
fi
OPENWRT_DIR=$(cd "$OPENWRT_DIR" && pwd) || exit 2
MK="$OPENWRT_DIR/package/kernel/mac80211/Makefile"

PKG_VER=$(sed -n 's/^PKG_SOURCE_VERSION:= *//p' "$MK" | head -1)
PKG_HASH=$(sed -n 's/^PKG_HASH:= *//p' "$MK" | head -1)
[ -n "$PKG_VER" ] || { echo "::error::PKG_SOURCE_VERSION not found" >&2; exit 2; }
SRC_URL="https://github.com/openwrt/backports/releases/download/backports-v${PKG_VER}/backports-${PKG_VER}.tar.zst"
TARBALL="$OPENWRT_DIR/dl/backports-${PKG_VER}.tar.zst"

echo "== patch check: backports-$PKG_VER ($OPENWRT_DIR)"

mkdir -p "$OPENWRT_DIR/dl"
fetch() { curl -fL --retry 3 --retry-delay 2 --connect-timeout 20 --max-time 560 -o "$TARBALL" "$SRC_URL"; }
if [ ! -f "$TARBALL" ]; then
	fetch || { echo "::error::download failed" >&2; exit 2; }
fi
if [ -n "$PKG_HASH" ]; then
	actual=$(sha256sum "$TARBALL" | awk '{print $1}')
	if [ "$actual" != "$PKG_HASH" ]; then
		rm -f "$TARBALL"
		fetch || { echo "::error::re-download failed" >&2; exit 2; }
		actual=$(sha256sum "$TARBALL" | awk '{print $1}')
		[ "$actual" = "$PKG_HASH" ] || { echo "::error::hash mismatch" >&2; exit 2; }
	fi
fi

WORK=$(mktemp -d) || exit 2
trap 'rm -rf "$WORK"' EXIT
zstd -dc "$TARBALL" | tar -x -C "$WORK" || { echo "::error::extract failed" >&2; exit 2; }
TREE="$WORK/backports-$PKG_VER"
[ -d "$TREE" ] || TREE=$(find "$WORK" -mindepth 1 -maxdepth 1 -type d | head -1)

SUBDIRS=$(sed -n 's/.*PatchDir,\$(PKG_BUILD_DIR),\$(PATCH_DIR)\/\([A-Za-z0-9_]*\),.*/\1/p' "$MK")
[ -n "$SUBDIRS" ] || SUBDIRS="build subsys ath ath5k ath9k ath10k ath11k ath12k rt2x00 mt7601u mwl brcm rtl"
PATCHES=()
for sub in $SUBDIRS; do
	[ -d "$OPENWRT_DIR/package/kernel/mac80211/patches/$sub" ] || continue
	while IFS= read -r p; do PATCHES+=("$p"); done < <(ls "$OPENWRT_DIR/package/kernel/mac80211/patches/$sub"/*.patch 2>/dev/null | sort)
done
[ ${#PATCHES[@]} -gt 0 ] || { echo "::error::no patches found" >&2; exit 2; }

ok=0; fuzz=0; fail=0
for p in "${PATCHES[@]}"; do
	rel="${p#"$OPENWRT_DIR"/}"
	out=$(patch -p1 --batch --forward --no-backup-if-mismatch --fuzz=2 -d "$TREE" -i "$p" 2>&1)
	rc=$?
	if [ $rc -eq 0 ] && ! echo "$out" | grep -q "fuzz"; then
		echo "  [OK]   $rel"; ok=$((ok+1))
	elif [ $rc -eq 0 ]; then
		echo "  [FUZZ] $rel"
		echo "::warning::patch applied with fuzz: $rel"
		echo "$out" | grep -i "fuzz\|offset" | sed 's/^/         /'
		fuzz=$((fuzz+1))
	else
		echo "  [FAIL] $rel"
		echo "::error::patch failed: $rel"
		echo "$out" | grep -iv "^patching file" | head -12 | sed 's/^/         /'
		fail=$((fail+1))
	fi
done

echo "== OK=$ok FUZZ=$fuzz FAIL=$fail / ${#PATCHES[@]}"
if [ "$fail" -gt 0 ]; then
	if [ "${ALLOW_PATCH_FAIL:-0}" = "1" ]; then
		echo "!! ALLOW_PATCH_FAIL=1"
		exit 0
	fi
	echo "::error::$fail patch(es) failed on backports-$PKG_VER"
	exit 1
fi

exit 0

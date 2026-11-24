#!/usr/bin/env bash
set -u

OW="${1:-}"
if [ -z "$OW" ]; then
	for d in openwrt .; do
		[ -f "$d/target/linux/generic/kernel-7.3" ] && OW="$d" && break
	done
fi
if [ -z "$OW" ] || [ ! -f "$OW/target/linux/generic/kernel-7.3" ]; then
	echo "::error::openwrt tree / kernel-7.3 not found" >&2
	exit 2
fi
OW=$(cd "$OW" && pwd) || exit 2

KVF="$OW/target/linux/generic/kernel-7.3"
SUB=$(sed -n 's/^LINUX_VERSION-7\.3 *= *\.//p' "$KVF" | head -1)
HASH=$(sed -n "s/^LINUX_KERNEL_HASH-7\.3\.$SUB *= *//p" "$KVF" | head -1)
case "$SUB" in ''|*[!0-9]*) echo "::error::cannot parse kernel sublevel from $KVF" >&2; exit 2;; esac
KVER="7.3.$SUB"
PATCH_DIR="$GITHUB_WORKSPACE/scripts/sy108/target/linux/sprd/patches-7.3"
[ -d "$PATCH_DIR" ] || PATCH_DIR="$OW/../scripts/sy108/target/linux/sprd/patches-7.3"
[ -d "$PATCH_DIR" ] || PATCH_DIR="$(dirname "$OW")/scripts/sy108/target/linux/sprd/patches-7.3"
if [ ! -d "$PATCH_DIR" ]; then
	echo "::error::patches-7.3 dir not found" >&2
	exit 2
fi

echo "== kernel patch check: $KVER against $(ls "$PATCH_DIR"/*.patch 2>/dev/null | wc -l) patches"

TARBALL="$OW/dl/linux-$KVER.tar.xz"
DL=0
if [ ! -f "$TARBALL" ]; then
	echo "   dl/ 未缓存 $KVER，临时下载..."
	mkdir -p "$OW/dl"
	URL="https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-$KVER.tar.xz"
	curl -fL --retry 3 --retry-delay 2 --connect-timeout 20 --max-time 560 -o "$TARBALL" "$URL" || {
		echo "::error::kernel tarball download failed" >&2; exit 2;
	}
	DL=1
fi

if [ -n "$HASH" ]; then
	actual=$(sha256sum "$TARBALL" | awk '{print $1}')
	if [ "$actual" != "$HASH" ]; then
		echo "::error::kernel hash mismatch on linux-$KVER: kernel-7.3 hash=$HASH actual=$actual" >&2
		echo "::error::自动跟随写入的内核 hash 与实际源码不符，已阻断编译" >&2
		exit 2
	fi
fi

WORK=$(mktemp -d) || exit 2
trap 'rm -rf "$WORK"' EXIT
tar -xf "$TARBALL" -C "$WORK" || { echo "::error::extract failed" >&2; exit 2; }
TREE="$WORK/linux-$KVER"
[ -d "$TREE" ] || TREE=$(find "$WORK" -mindepth 1 -maxdepth 1 -type d | head -1)

ok=0; fuzz=0; fail=0
for p in $(ls "$PATCH_DIR"/*.patch 2>/dev/null | sort); do
	rel=$(basename "$p")
	out=$(patch -p1 --batch --forward --no-backup-if-mismatch --fuzz=2 -d "$TREE" -i "$p" 2>&1)
	rc=$?
	if [ $rc -eq 0 ] && ! echo "$out" | grep -qiE 'fuzz|offset'; then
		echo "  [OK]   $rel"; ok=$((ok+1))
	elif [ $rc -eq 0 ]; then
		echo "  [FUZZ] $rel"
		echo "::warning::kernel patch applied with fuzz/offset: $rel"
		echo "$out" | grep -iE 'fuzz|offset' | sed 's/^/         /'
		fuzz=$((fuzz+1))
	else
		echo "  [FAIL] $rel"
		echo "::error::kernel patch failed on linux-$KVER: $rel"
		echo "$out" | grep -iE 'Hunk|FAILED|can.t find|malformed|Reversed' | head -12 | sed 's/^/         /'
		fail=$((fail+1))
	fi
done

echo "== OK=$ok FUZZ=$fuzz FAIL=$fail on linux-$KVER"
if [ "$fail" -gt 0 ]; then
	echo "::error::$fail kernel patch(es) failed on linux-$KVER. 自动跟随到的新版本与补丁不兼容，请回退内核版本或重同步补丁。"
	exit 1
fi

PROCD_MK="$OW/package/system/procd/Makefile"
PROCD_VER=$(sed -n 's/^PKG_SOURCE_VERSION:= *//p' "$PROCD_MK" 2>/dev/null | head -1)
P991="$GITHUB_WORKSPACE/scripts/sy108/package/system/procd/patches/991-jail-pidfile-status.patch"
[ -f "$P991" ] || P991="$(dirname "$OW")/scripts/sy108/package/system/procd/patches/991-jail-pidfile-status.patch"
if [ -n "$PROCD_VER" ] && [ -f "$P991" ]; then
	echo "== procd patch check: $PROCD_VER"
	PWORK=$(mktemp -d) || exit 2
	curl -fL --retry 3 --connect-timeout 20 --max-time 300 -o "$PWORK/procd.tar.gz" \
		"https://github.com/openwrt/procd/archive/$PROCD_VER.tar.gz" || {
		echo "::error::procd source download failed" >&2
		rm -rf "$PWORK"; exit 2
	}
	tar -xzf "$PWORK/procd.tar.gz" -C "$PWORK" || { echo "::error::procd extract failed" >&2; rm -rf "$PWORK"; exit 2; }
	PTREE=$(find "$PWORK" -mindepth 1 -maxdepth 1 -type d | head -1)
	pout=$(patch -p1 --batch --forward --no-backup-if-mismatch --fuzz=0 -d "$PTREE" -i "$P991" 2>&1)
	prc=$?
	if [ $prc -eq 0 ] && ! echo "$pout" | grep -qiE 'fuzz|offset'; then
		echo "  [OK]   991-jail-pidfile-status.patch (procd)"
	elif [ $prc -eq 0 ]; then
		echo "  [FUZZ] 991-jail-pidfile-status.patch (procd)"
		echo "::warning::procd patch applied with fuzz/offset: 991-jail-pidfile-status.patch"
	else
		echo "  [FAIL] 991-jail-pidfile-status.patch (procd)"
		echo "::error::procd patch failed: 991-jail-pidfile-status.patch"
		echo "$pout" | grep -iE 'Hunk|FAILED|can.t find|malformed' | head -12 | sed 's/^/         /'
		rm -rf "$PWORK"; exit 1
	fi
	rm -rf "$PWORK"
fi
exit 0

#!/usr/bin/env bash
set -u

OW="${1:-}"
if [ -z "$OW" ]; then
	for d in openwrt /mnt/openwrt .; do
		[ -f "$d/target/linux/generic/kernel-7.3" ] && OW="$d" && break
	done
fi
if [ -z "$OW" ] || [ ! -f "$OW/target/linux/generic/kernel-7.3" ]; then
	echo "::error::openwrt tree / kernel-7.3 not found" >&2
	exit 2
fi
OW=$(cd "$OW" && pwd) || exit 2

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd) || exit 2
SCRIPTS="${2:-$SCRIPT_DIR}"
if [ ! -d "$SCRIPTS/target/linux/msm89xx/patches-7.3" ]; then
	echo "::error::scripts/msm89xx patch dirs not found under $SCRIPTS" >&2
	exit 2
fi

KVF="$OW/target/linux/generic/kernel-7.3"
SUB=$(sed -n 's/^LINUX_VERSION-7\.3 *= *\.//p' "$KVF" | head -1)
HASH=$(sed -n "s/^LINUX_KERNEL_HASH-7\.3\.$SUB *= *//p" "$KVF" | head -1)
case "$SUB" in ''|*[!0-9]*) echo "::error::cannot parse kernel sublevel from $KVF" >&2; exit 2;; esac
KVER="7.3.$SUB"

TARBALL="$OW/dl/linux-$KVER.tar.xz"
if [ ! -f "$TARBALL" ]; then
	echo "   dl/ cache miss for $KVER, downloading..."
	mkdir -p "$OW/dl"
	URL="https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-$KVER.tar.xz"
	curl -fL --retry 3 --retry-delay 2 --connect-timeout 20 --max-time 560 -o "$TARBALL" "$URL" || {
		echo "::error::kernel tarball download failed" >&2; exit 2;
	}
fi
if [ -n "$HASH" ]; then
	actual=$(sha256sum "$TARBALL" | awk '{print $1}')
	if [ "$actual" != "$HASH" ]; then
		echo "::error::kernel hash mismatch on linux-$KVER: kernel-7.3 hash=$HASH actual=$actual" >&2
		echo "::error::auto-followed kernel hash does not match source, blocked" >&2
		exit 2
	fi
fi

WORK=$(mktemp -d) || exit 2
trap 'rm -rf "$WORK"' EXIT

ok=0; fuzz=0; fail=0

apply_group() {
	local tree="$1"; shift
	local p rel out rc
	for p in "$@"; do
		[ -f "$p" ] || continue
		rel="${p#"$SCRIPTS/"}"
		out=$(patch -p1 --batch --forward --no-backup-if-mismatch --fuzz=2 -d "$tree" -i "$p" 2>&1)
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
}

echo "== kernel patch check: $KVER"

mkdir -p "$WORK/t1"
tar -xf "$TARBALL" -C "$WORK/t1" || { echo "::error::extract failed" >&2; exit 2; }
T1=$(find "$WORK/t1" -mindepth 1 -maxdepth 1 -type d | head -1)
HACKS=$(ls "$SCRIPTS/target/linux/generic/hack-7.3"/*.patch 2>/dev/null | sort)
TP=$(ls "$SCRIPTS/target/linux/msm89xx/patches-7.3"/*.patch 2>/dev/null | sort)
echo "== target/linux: $(echo "$HACKS" | grep -c .) hack + $(echo "$TP" | grep -c .) patches"
apply_group "$T1" $HACKS $TP

mkdir -p "$WORK/t2"
tar -xf "$TARBALL" -C "$WORK/t2" || { echo "::error::extract failed" >&2; exit 2; }
T2=$(find "$WORK/t2" -mindepth 1 -maxdepth 1 -type d | head -1)
TC=$(ls "$SCRIPTS/toolchain/kernel-headers/patches"/*.patch 2>/dev/null | sort)
echo "== toolchain/kernel-headers: $(echo "$TC" | grep -c .) patches"
apply_group "$T2" $TC

echo "== OK=$ok FUZZ=$fuzz FAIL=$fail on linux-$KVER"
if [ "$fail" -gt 0 ]; then
	echo "::error::$fail kernel patch(es) failed on linux-$KVER, blocked" >&2
	exit 1
fi
exit 0

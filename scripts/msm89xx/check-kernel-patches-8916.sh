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

MAC80211_MK="$SCRIPTS/package/kernel/mac80211/Makefile"
BP_VER=$(sed -n 's/^PKG_SOURCE_VERSION *:= *//p' "$MAC80211_MK" 2>/dev/null | head -1)
BP_HASH=$(sed -n 's/^PKG_HASH *:= *//p' "$MAC80211_MK" 2>/dev/null | head -1)
ATH_DIR="$SCRIPTS/package/kernel/mac80211/patches/ath"
ATH=$(ls "$ATH_DIR"/*.patch 2>/dev/null | sort)
case "$BP_VER" in ''|*[!0-9.]*) echo "::error::cannot parse backports version from mac80211 Makefile" >&2; exit 2;; esac
BP_TARBALL="$OW/dl/backports-$BP_VER.tar.zst"
if [ ! -f "$BP_TARBALL" ]; then
	echo "   dl/ cache miss for backports-$BP_VER, downloading..."
	mkdir -p "$OW/dl"
	BP_URL="https://github.com/openwrt/backports/releases/download/backports-v$BP_VER/backports-$BP_VER.tar.zst"
	curl -fL --retry 3 --retry-delay 2 --connect-timeout 20 --max-time 560 -o "$BP_TARBALL" "$BP_URL" || {
		echo "::error::backports tarball download failed" >&2; exit 2;
	}
fi
if [ -n "$BP_HASH" ]; then
	bp_actual=$(sha256sum "$BP_TARBALL" | awk '{print $1}')
	if [ "$bp_actual" != "$BP_HASH" ]; then
		echo "::error::backports hash mismatch: Makefile=$BP_HASH actual=$bp_actual" >&2
		exit 2
	fi
fi
command -v zstd >/dev/null 2>&1 || tar --zstd --version >/dev/null 2>&1 || {
	echo "::error::zstd required to extract backports tarball" >&2; exit 2;
}
mkdir -p "$WORK/t3"
tar --zstd -xf "$BP_TARBALL" -C "$WORK/t3" || { echo "::error::backports extract failed" >&2; exit 2; }
T3=$(find "$WORK/t3" -mindepth 1 -maxdepth 1 -type d | head -1)
echo "== package/kernel/mac80211 (backports-$BP_VER): $(echo "$ATH" | grep -c .) ath patches"
apply_group "$T3" $ATH

check_pkg_patch() {
	local mk="$1" verkey="$2" hashkey="$3" tname="$4" urlbase="$5" patchfile="$6" label="$7"
	local ver hsh tar f actual tree out rc
	[ -f "$mk" ] || { echo "  [FAIL] $label: Makefile missing: $mk" >&2; fail=$((fail+1)); return; }
	[ -f "$patchfile" ] || { echo "  [FAIL] $label: patch missing: $patchfile" >&2; fail=$((fail+1)); return; }
	ver=$(sed -n "s/^${verkey} *:= *//p" "$mk" | head -1)
	case "$ver" in ''|*[!0-9.]*) echo "  [FAIL] $label: cannot parse $verkey from $mk" >&2; fail=$((fail+1)); return;; esac
	hsh=$(sed -n "s/^${hashkey} *:= *//p" "$mk" | head -1)
	tar="$OW/dl/$tname"
	if [ ! -f "$tar" ]; then
		echo "   dl/ cache miss for $tname, downloading..."
		mkdir -p "$OW/dl"
		curl -fL --retry 3 --retry-delay 2 --connect-timeout 20 --max-time 560 -o "$tar" "$urlbase/$tname" || {
			echo "  [FAIL] $label: tarball download failed" >&2; fail=$((fail+1)); return;
		}
	fi
	if [ -n "$hsh" ]; then
		actual=$(sha256sum "$tar" | awk '{print $1}')
		if [ "$actual" != "$hsh" ]; then
			echo "  [FAIL] $label: hash mismatch: Makefile=$hsh actual=$actual" >&2
			fail=$((fail+1)); return
		fi
	fi
	mkdir -p "$WORK/$label"
	case "$tname" in
		*.tar.zst) tar --zstd -xf "$tar" -C "$WORK/$label" || { echo "  [FAIL] $label: extract failed" >&2; fail=$((fail+1)); return; };;
		*) tar -xf "$tar" -C "$WORK/$label" || { echo "  [FAIL] $label: extract failed" >&2; fail=$((fail+1)); return; };;
	esac
	tree=$(find "$WORK/$label" -mindepth 1 -maxdepth 1 -type d | head -1)
	out=$(patch -p1 --batch --forward --no-backup-if-mismatch --fuzz=2 -d "$tree" -i "$patchfile" 2>&1); rc=$?
	if [ $rc -eq 0 ] && ! echo "$out" | grep -qiE 'fuzz|offset'; then
		echo "  [OK]   $label: $(basename "$patchfile")"; ok=$((ok+1))
	elif [ $rc -eq 0 ]; then
		echo "  [FUZZ] $label: $(basename "$patchfile")"
		echo "::warning::$label patch applied with fuzz/offset"
		echo "$out" | grep -iE 'fuzz|offset' | sed 's/^/         /'
		fuzz=$((fuzz+1))
	else
		echo "  [FAIL] $label: $(basename "$patchfile")"
		echo "::error::$label patch failed, blocked"
		echo "$out" | grep -iE 'Hunk|FAILED|can.t find|malformed|Reversed' | head -12 | sed 's/^/         /'
		fail=$((fail+1))
	fi
}

echo "== package patches: bluez / pulseaudio"
BLUEZ_MK="$OW/feeds/packages/utils/bluez/Makefile"
PA_MK="$OW/feeds/packages/sound/pulseaudio/Makefile"

BV=$(sed -n 's/^PKG_VERSION *:= *//p' "$BLUEZ_MK" 2>/dev/null | head -1)
BLUEZ_TAR="bluez-$BV.tar.xz"
check_pkg_patch "$BLUEZ_MK" PKG_VERSION PKG_HASH "$BLUEZ_TAR" \
	"https://www.kernel.org/pub/linux/bluetooth" \
	"$SCRIPTS/patches/300-bluez-main-conf-secure-connections-off.patch" bluez

PV=$(sed -n 's/^PKG_VERSION *:= *//p' "$PA_MK" 2>/dev/null | head -1)
PD=$(sed -n 's/^PKG_SOURCE_DATE *:= *//p' "$PA_MK" 2>/dev/null | head -1)
PSV=$(sed -n 's/^PKG_SOURCE_VERSION *:= *//p' "$PA_MK" 2>/dev/null | head -1)
PABBR=$(printf '%.8s' "$PSV")
PA_TAR="pulseaudio-$PV-$PD-$PABBR.tar.zst"
check_pkg_patch "$PA_MK" PKG_VERSION PKG_MIRROR_HASH "$PA_TAR" \
	"https://sources.openwrt.org" \
	"$SCRIPTS/patches/005-pulseaudio-system-pa-auth-anonymous.patch" pulseaudio
check_pkg_patch "$PA_MK" PKG_VERSION PKG_MIRROR_HASH "$PA_TAR" \
	"https://sources.openwrt.org" \
	"$SCRIPTS/patches/007-pulseaudio-system-pa-disable-module-detect.patch" pulseaudio007

echo "== sound Kconfig patch presence"
P0105="$SCRIPTS/target/linux/msm89xx/patches-7.3/0105-sound-core-add-prompt-to-snd-hwdep-and-compress.patch"
if [ ! -f "$P0105" ]; then
	echo "  [FAIL] 0105-sound-core-add-prompt-to-snd-hwdep-and-compress.patch missing"
	echo "::error::0105 sound Kconfig patch not found under $SCRIPTS, kmod-sound-core will hit snd-hwdep.ko is missing"
	fail=$((fail+1))
elif ! grep -q 'tristate "ALSA hwdep interface"' "$P0105"; then
	echo "  [FAIL] 0105 patch has no SND_HWDEP prompt hunk"
	echo "::error::0105 sound Kconfig patch malformed: SND_HWDEP prompt hunk absent"
	fail=$((fail+1))
elif ! grep -q 'tristate "ALSA compressed offload support"' "$P0105"; then
	echo "  [FAIL] 0105 patch has no SND_COMPRESS_OFFLOAD prompt hunk"
	echo "::error::0105 sound Kconfig patch malformed: SND_COMPRESS_OFFLOAD prompt hunk absent"
	fail=$((fail+1))
else
	echo "  [OK]   0105 both prompt hunks present, apply verified in target group above"
	ok=$((ok+1))
fi

echo "== OK=$ok FUZZ=$fuzz FAIL=$fail on linux-$KVER"
if [ "$fail" -gt 0 ]; then
	echo "::error::$fail kernel patch(es) failed on linux-$KVER, blocked" >&2
	exit 1
fi
exit 0

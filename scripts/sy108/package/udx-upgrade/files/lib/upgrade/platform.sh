#!/bin/sh
REQUIRE_IMAGE_METADATA=1
REQUIRE_IMAGE_SIGNATURE=0
CI_UBIPART=ubipac
CI_KERNPART=boot
RAMFS_COPY_BIN="$RAMFS_COPY_BIN /usr/libexec/udx-boot-state ubiupdatevol ubiblock ubirename"

sy108_volume() {
	local ubi volume
	ubi=$(nand_find_ubi "$CI_UBIPART") || return 1
	volume=$(nand_find_volume "$ubi" "$1") || return 1
	[ -n "$volume" ] || return 1
	[ "$(cat /sys/class/ubi/$volume/type)" = dynamic ] || return 1
	printf '%s\n' "$volume"
}

sy108_volume_bytes() {
	local blocks leb
	read -r blocks < "/sys/class/ubi/$1/reserved_ebs" || return 1
	read -r leb < "/sys/class/ubi/$1/usable_eb_size" || return 1
	case "$blocks:$leb" in *[!0-9:]*|:*|*:) return 1;; esac
	[ "$blocks" -gt 0 ] && [ "$leb" -gt 0 ] || return 1
	printf '%s\n' "$((blocks * leb))"
}

sy108_system_volumes() {
	local root data ubi
	ubi=$(nand_find_ubi "$CI_UBIPART") || return 1
	root=$(nand_find_volume "$ubi" rootfs) || root=
	data=$(nand_find_volume "$ubi" rootfs_data) || data=
	if [ -n "$root" ] && [ -n "$data" ]; then
		root=$(sy108_volume rootfs) && data=$(sy108_volume rootfs_data) || return 1
		printf 'installed %s %s\n' "$root" "$data"
	elif [ -z "$root" ] && [ -z "$data" ] && [ -f /etc/udx-recovery-root ]; then
		root=$(sy108_volume system) && data=$(sy108_volume userdata) || return 1
		printf 'factory %s %s\n' "$root" "$data"
	else
		return 1
	fi
}

sy108_check_image() (
	[ "${FWTOOL_DEVICE_MATCH:-1}" = 0 ] || return 74
	set -o pipefail
	local directory root_bytes boot root data layout boot_capacity root_capacity
	directory=$(mktemp -d /tmp/sy108-image-check.XXXXXX) || return 74
	trap 'rm -rf "$directory"' EXIT
	[ "$(tar -tf "$1" | awk '$0=="sysupgrade-sy108/kernel" {k++} $0=="sysupgrade-sy108/root" {r++} END {print k ":" r}')" = 1:1 ] || return 74
	tar -xOf "$1" sysupgrade-sy108/kernel > "$directory/kernel" || return 74
	root_bytes=$(tar -xOf "$1" sysupgrade-sy108/root | wc -c) || return 74
	tar -xOf "$1" sysupgrade-sy108/root | {
		head -c 96 > "$directory/root-header"
		cat >/dev/null
	} || return 74
	boot=$(sy108_volume boot) || return 74
	set -- $2
	[ "$#" = 3 ] || return 74
	layout=$1; root=$2; data=$3
	[ "$root" = ubi0_21 ] || return 74
	boot_capacity=$(sy108_volume_bytes "$boot") || return 74
	root_capacity=$(sy108_volume_bytes "$root") || return 74
	sy108_volume_bytes "$data" >/dev/null || return 74
	/usr/libexec/udx-image-check "$directory/kernel" "$directory/root-header" "$root_bytes" "$boot_capacity" "$root_capacity" || return 74
)

platform_check_image() {
	local volumes
	volumes=$(sy108_system_volumes) || return 74
	case "$volumes" in
		factory\ *)
			if type notify_firmware_no_backup >/dev/null 2>&1; then
				notify_firmware_no_backup
			fi
			;;
	esac
	sy108_check_image "$1" "$volumes"
}

platform_prepare_upgrade() {
	/usr/libexec/udx-prepare-upgrade
}

platform_quiesce_usb() {
	local gadget=/sys/kernel/config/usb_gadget/udx-usb/UDC
	local udc=/sys/class/udc/29100000.usb
	local pam=/sys/bus/platform/devices/2b300000.pamu3/offload_state
	local bound state
	if [ -f "$gadget" ]; then
		read -r bound < "$gadget" || return 1
		case "$bound" in
			29100000.usb) printf '\n' > "$gadget" || return 1 ;;
			'') ;;
			*)
				echo 'Unexpected USB gadget controller' >&2
				return 1
				;;
		esac
	fi
	if [ -f "$udc/state" ]; then
		read -r state < "$udc/state" || return 1
		[ "$state" = 'not attached' ] || {
			echo 'USB controller did not detach' >&2
			return 1
		}
	fi
	if [ -f "$pam" ]; then
		read -r state < "$pam" || return 1
		case "$state" in
			'clock=0 prepared=0 claimed=0 running=0 connected=0 requested=0 '*) ;;
			*)
				echo 'USB accelerator still owns DMA resources' >&2
				return 1
				;;
		esac
	fi
}

platform_do_upgrade() {
	local image="$1" boot root data layout ubi kernel_bytes root_bytes boot_capacity root_capacity
	set -o pipefail
	boot=$(sy108_volume boot) || exit 1
	set -- $(sy108_system_volumes) || exit 1
	[ "$#" = 3 ] || exit 1
	layout=$1; root=$2; data=$3
	[ "$root" = ubi0_21 ] || exit 1
	kernel_bytes=$(tar -xOf "$image" sysupgrade-sy108/kernel | wc -c) || exit 1
	root_bytes=$(tar -xOf "$image" sysupgrade-sy108/root | wc -c) || exit 1
	boot_capacity=$(sy108_volume_bytes "$boot") || exit 1
	root_capacity=$(sy108_volume_bytes "$root") || exit 1
	[ "$kernel_bytes" -gt 0 ] && [ "$kernel_bytes" -le "$boot_capacity" ] || exit 1
	[ "$root_bytes" -gt 0 ] && [ "$root_bytes" -le "$root_capacity" ] || exit 1
	if [ "$layout" = factory ] && [ -n "$UPGRADE_BACKUP" ]; then
		echo 'First installation from factory layout requires sysupgrade -n.' >&2
		exit 1
	fi
	platform_quiesce_usb || exit 1
	if [ -e "/sys/class/block/ubiblock${root#ubi}" ]; then
		ubiblock -r "/dev/$root" || exit 1
	fi
	[ ! -e "/sys/class/block/ubiblock${root#ubi}" ] || exit 1
	if [ "$layout" = factory ]; then
		ubi=$(nand_find_ubi "$CI_UBIPART") || exit 1
		ubirename "/dev/$ubi" system rootfs userdata rootfs_data || exit 1
	fi
	tar -xOf "$image" sysupgrade-sy108/kernel | ubiupdatevol "/dev/$boot" -s "$kernel_bytes" - || exit 1
	tar -xOf "$image" sysupgrade-sy108/root | ubiupdatevol "/dev/$root" -s "$root_bytes" - || exit 1
	ubiupdatevol "/dev/$data" -t || exit 1
	if [ -z "$UPGRADE_BACKUP" ]; then
		/usr/libexec/udx-boot-state trial || exit 1
	fi
}

platform_copy_config() {
	nand_restore_config "$UPGRADE_BACKUP" || exit 1
	/usr/libexec/udx-boot-state trial || exit 1
}

if [ -f /etc/udx-recovery-root ]; then
	platform_restore_backup() {
		/usr/libexec/udx-recovery-config restore "$CONF_RESTORE"
	}
	platform_create_backup() {
		local volumes
		volumes=$(sy108_system_volumes) || return 1
		case "$volumes" in
			factory\ *) echo 'No installed OpenWrt configuration; use sysupgrade -n for first installation.' >&2; return 1;;
		esac
		/usr/libexec/udx-recovery-config backup "$1" "${SKIP_UNCHANGED:-0}" "${SAVE_INSTALLED_PKGS:-0}" "${SAVE_OVERLAY_PATH:-}"
	}
fi

#!/bin/sh

PART_NAME=firmware
REQUIRE_IMAGE_METADATA=1
RAMFS_COPY_DATA="/lib/functions.sh /lib/upgrade/common.sh /lib/upgrade/fwtool.sh /lib/upgrade/luci-add-conffiles.sh /lib/upgrade/platform.sh /lib/upgrade/tar.sh"

platform_check_image() {
    local fw_image="$1"
    local board_dir

    board_dir=$(tar tf "$fw_image" 2>/dev/null | grep -m 1 '^sysupgrade-.*/$')

    if [ -z "$board_dir" ]; then
        echo "Invalid syupgrade file: $fw_image"
        return 1
    fi

    board_dir=${board_dir%/}

    if ! tar tf "$fw_image" 2>/dev/null | grep -q "^${board_dir}/CONTROL$"; then
        echo "Invalid syupgrade file: missing CONTROL"
        return 1
    fi

    if ! tar tf "$fw_image" 2>/dev/null | grep -q "^${board_dir}/kernel$"; then
        echo "Invalid syupgrade file: missing kernel"
        return 1
    fi

    if ! tar tf "$fw_image" 2>/dev/null | grep -q "^${board_dir}/root$"; then
        echo "Invalid syupgrade file: missing root"
        return 1
    fi

    return 0
}

platform_do_upgrade() {
    local tar_file="$1"
    local boot_part rootfs_part
    local keep_config=0

    [ -n "$UPGRADE_BACKUP" ] && [ -f "$UPGRADE_BACKUP" ] && keep_config=1

    local board_dir=$(tar tf "$tar_file" | grep -m 1 '^sysupgrade-.*/$')
    board_dir=${board_dir%/}

    [ -n "$board_dir" ] || {
        echo "sysupgrade: cannot find board directory"
        return 1
    }

    boot_part=$(find_mmc_part "boot")
    rootfs_part=$(find_mmc_part "rootfs")

    [ -z "$boot_part" ] && {
        echo "sysupgrade: cannot find 'boot' partition"
        return 1
    }

    [ -z "$rootfs_part" ] && {
        echo "sysupgrade: cannot find 'rootfs' partition"
        return 1
    }

    echo "sysupgrade: writing kernel to $boot_part"
    tar -xOf "$tar_file" "${board_dir}/kernel" 2>/dev/null |
        dd of="$boot_part" bs=4096 conv=fsync || {
            echo "sysupgrade: failed to write kernel"
            return 1
        }

    echo "sysupgrade: writing rootfs to $rootfs_part"
    tar -xOf "$tar_file" "${board_dir}/root" 2>/dev/null |
        dd of="$rootfs_part" bs=4096 conv=fsync || {
            echo "sysupgrade: failed to write rootfs"
            return 1
        }

    sync

    if [ "$keep_config" -ne 1 ]; then
        echo "sysupgrade: not keeping config, wiping overlay"
        overlay_dev=$(find_mmc_part "rootfs_data")
        [ -z "$overlay_dev" ] && overlay_dev=/dev/mmcblk0p15
        if [ -b "$overlay_dev" ]; then
            dd if=/dev/zero of="$overlay_dev" bs=1M count=1 2>/dev/null
            sync
            echo "sysupgrade: overlay wiped"
        fi
    else
        echo "sysupgrade: keeping config, skip overlay wipe"
    fi
}

platform_pre_upgrade() {
    mkdir -p /overlay/upper 2>/dev/null

    if [ -n "$UPGRADE_BACKUP" ] && [ -f "$UPGRADE_BACKUP" ]; then
        echo "pre_upgrade: keeping config+packages, skip upper wipe"
        cp -f "$UPGRADE_BACKUP" "/overlay/upper/$BACKUP_FILE" 2>/dev/null
    else
        echo "pre_upgrade: not keeping config, wiping upper"
        rm -fr /overlay/upper/* /overlay/upper/.* 2>/dev/null
    fi
}

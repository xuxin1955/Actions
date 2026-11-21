# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk
ARCH:=aarch64
BOARD:=sprd
BOARDNAME:=Spreadtrum UDX710
SUBTARGETS:=udx710
FEATURES:=fpu nand rtc squashfs ramdisk usb
KERNEL_PATCHVER:=7.3
KERNELNAME:=Image dtbs
include $(INCLUDE_DIR)/target.mk
ifeq ($(CONFIG_SY108_RECOVERY),y)
LINUX_KCONFIG_LIST += $(PLATFORM_SUBDIR)/config-recovery
endif
DEFAULT_PACKAGES += udx-usb udx-runtime udx-upgrade
$(eval $(call BuildTarget))

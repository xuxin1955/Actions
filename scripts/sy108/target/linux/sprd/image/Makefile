# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/image.mk
DTC_FLAGS := $(filter-out -Wno-graph_child_address,$(DTC_FLAGS))

SY108_CAPSULE := $(STAGING_DIR_HOST)/share/sy108-image/tools/image/capsule.py
define Image/Prepare
	$(LN) /var/run $(TARGET_DIR)/run
	$(if $(CONFIG_SY108_RECOVERY),$(INSTALL_BIN) $(GENERIC_PLATFORM_DIR)/other-files/init $(TARGET_DIR)/init)
endef
define Build/sy108-capsule
	python3 $(SY108_CAPSULE) \
		--image $@ --dtb $(KDIR)/image-udx710-sy108.dtb \
		--target openwrt --output $@.capsule
	mv $@.capsule $@
endef

define Device/sy108
  DEVICE_MODEL := SY108
  DEVICE_VARIANT := UDX710
  DEVICE_DTS := sprd/udx710-sy108
  SUPPORTED_DEVICES := sprd,sy108
  KERNEL := kernel-bin | sy108-capsule
  KERNEL_INITRAMFS := kernel-bin
  KERNEL_INSTALL := 1
  KERNEL_NAME := Image
  IMAGES := rootfs.squashfs sysupgrade.bin
  IMAGE/rootfs.squashfs := append-rootfs
  IMAGE/sysupgrade.bin := sysupgrade-tar | append-metadata
  DEVICE_PACKAGES := udx-usb udx-runtime udx-upgrade
  ifeq ($(CONFIG_SY108_RECOVERY),y)
    DEVICE_DTS := sprd/udx710-sy108-recovery
    KERNEL := kernel-bin
    IMAGES :=
  endif
endef
TARGET_DEVICES += sy108
$(eval $(call BuildImage))

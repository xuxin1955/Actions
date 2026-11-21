include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=udx-pmsys
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef
Build/Compile =

define KernelPackage/udx710-pmsys
  SUBMENU:=Other modules
  TITLE:=SY108 PM_SYS remote processor
  DEPENDS:=@TARGET_sprd
  KCONFIG:=CONFIG_SPRD_UDX710_PMSYS
  FILES:=$(LINUX_DIR)/drivers/remoteproc/udx710-pmsys.ko
endef

define KernelPackage/udx710-pmsys/install
	$(INSTALL_DIR) $(1)/lib/preinit $(1)/lib/firmware/sprd
	$(INSTALL_BIN) $(CURDIR)/files/07_sy108-pmsys $(1)/lib/preinit/
	$(LN) /tmp/sy108-firmware/udx710-pmsys.bin $(1)/lib/firmware/sprd/udx710-pmsys.bin
endef
$(eval $(call KernelPackage,udx710-pmsys))

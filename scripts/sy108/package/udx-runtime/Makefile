include $(TOPDIR)/rules.mk

PKG_NAME:=udx-runtime
PKG_VERSION:=3.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
PKG_CONFIG_DEPENDS += CONFIG_SY108_RECOVERY SY108_BUILD_ID

include $(INCLUDE_DIR)/package.mk

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Package/udx-runtime
  SECTION:=base
  CATEGORY:=Base system
  TITLE:=SY108 boot, shutdown and recovery coordinator
  DEPENDS:=+kmod-udx710-pmsys +procd +ubus +libubus +libubox +ubi-utils +flock +udx-usb +ucode +ucode-mod-fs
endef

define Package/udx-runtime/config
  config SY108_RECOVERY
    bool "Build the independent SY108 RAM Recovery"
    default n
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CPPFLAGS) $(TARGET_CFLAGS) -std=gnu11 -Wall -Wextra \
	 $(CURDIR)/src/system/runtime.c $(TARGET_LDFLAGS) -lubus -lubox -o $(PKG_BUILD_DIR)/udx-runtime
	$(TARGET_CC) $(TARGET_CPPFLAGS) $(TARGET_CFLAGS) -std=gnu11 \
	 $(CURDIR)/src/system/verify-ubi-root.c $(TARGET_LDFLAGS) -o $(PKG_BUILD_DIR)/udx-verify-root
endef

define Package/udx-runtime/install
	$(INSTALL_DIR) $(1)
	$(CP) ./files/. $(1)/
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/usr/libexec
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/udx-runtime $(1)/usr/sbin/
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/udx-verify-root $(1)/usr/libexec/
	printf '180\n' > $(1)/etc/udx-trial-seconds
	printf 'sy108-%s\n' '$(SY108_BUILD_ID)' > $(1)/etc/udx-system-build
endef
ifeq ($(CONFIG_SY108_RECOVERY),y)
define Package/udx-runtime/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/init.d $(1)/etc/uci-defaults $(1)/etc/board.d
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/udx-runtime $(1)/usr/sbin/
	$(INSTALL_DIR) $(1)/usr/libexec
	$(INSTALL_BIN) $(CURDIR)/files/usr/libexec/sy108-system $(1)/usr/libexec/
	$(INSTALL_DIR) $(1)/etc/hotplug.d/serial
	$(INSTALL_BIN) $(CURDIR)/files/etc/hotplug.d/serial/90-sy108-mcu $(1)/etc/hotplug.d/serial/
	$(INSTALL_DIR) $(1)/lib/preinit
	$(INSTALL_BIN) $(CURDIR)/recovery/preinit $(1)/lib/preinit/05_sy108-recovery
	$(INSTALL_BIN) $(CURDIR)/files/etc/init.d/udx-runtime $(1)/etc/init.d/udx-runtime
	$(INSTALL_BIN) $(CURDIR)/recovery/identity $(1)/etc/uci-defaults/01-sy108-identity
	$(INSTALL_BIN) $(CURDIR)/recovery/defaults $(1)/etc/uci-defaults/95-sy108-recovery
	$(INSTALL_BIN) $(CURDIR)/files/etc/uci-defaults/02-sy108-system $(1)/etc/uci-defaults/02-sy108-system
	$(INSTALL_BIN) $(CURDIR)/files/etc/board.d/02-sy108-network $(1)/etc/board.d/
	$(INSTALL_BIN) $(CURDIR)/files/etc/uci-defaults/99-sy108-ports $(1)/etc/uci-defaults/
	$(INSTALL_DIR) $(1)/etc/sysctl.d
	$(INSTALL_DATA) $(CURDIR)/files/etc/sysctl.d/90-sy108.conf $(1)/etc/sysctl.d/
	printf 'OpenWrt Recovery RAM root\n' > $(1)/etc/udx-recovery-root
	printf '0\n' > $(1)/etc/udx-trial-seconds
	printf 'sy108-%s\n' '$(SY108_BUILD_ID)' > $(1)/etc/udx-system-build
endef
endif
$(eval $(call BuildPackage,udx-runtime))

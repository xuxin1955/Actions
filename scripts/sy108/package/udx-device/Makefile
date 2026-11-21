include $(TOPDIR)/rules.mk

PKG_NAME:=udx-device
PKG_VERSION:=3.1
PKG_RELEASE:=4
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Package/udx-device
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=SY108 native device RPC
  DEPENDS:=+libjson-c +libubus +libubox +rpcd +udx-cellular +udx-usb
endef

define Package/udx-device/conffiles
/etc/config/udx-hardware
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CPPFLAGS) $(TARGET_CFLAGS) -std=c11 \
	 -I$(CURDIR)/src \
	 $(CURDIR)/src/device.c $(CURDIR)/src/frequency.c $(CURDIR)/src/common.c \
	 $(TARGET_LDFLAGS) -ljson-c -lubus -lubox -o $(PKG_BUILD_DIR)/udx-device-rpc
endef

define Package/udx-device/install
	$(INSTALL_DIR) $(1)/usr/libexec/rpcd
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/udx-device-rpc $(1)/usr/libexec/rpcd/udx
	$(CP) ./files/. $(1)/
endef
$(eval $(call BuildPackage,udx-device))

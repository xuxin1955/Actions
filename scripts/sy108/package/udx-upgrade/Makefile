include $(TOPDIR)/rules.mk

PKG_NAME:=udx-upgrade
PKG_VERSION:=4.0
PKG_RELEASE:=2
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Package/udx-upgrade
 SECTION:=admin
 CATEGORY:=Administration
 TITLE:=SY108 platform upgrade support
 DEPENDS:=+udx-runtime +fwtool +libubus +libubox +libuci +libfdt +ubi-utils +fstools
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CPPFLAGS) $(TARGET_CFLAGS) -std=c11 -Os -Wall -Wextra -I$(CURDIR)/src/boot-format -I$(CURDIR)/src $(CURDIR)/src/boot-state.c $(CURDIR)/src/management-ready.c $(CURDIR)/src/boot-format/misc-state.c $(TARGET_LDFLAGS) -lubus -lubox -luci -o $(PKG_BUILD_DIR)/udx-boot-state
	$(TARGET_CC) $(TARGET_CPPFLAGS) $(TARGET_CFLAGS) -std=c11 -Os -Wall -Wextra -I$(CURDIR)/src/boot-format $(CURDIR)/src/image-check.c $(CURDIR)/src/boot-format/capsule.c $(CURDIR)/src/boot-format/misc-state.c $(TARGET_LDFLAGS) -lfdt -o $(PKG_BUILD_DIR)/udx-image-check
endef

define Package/udx-upgrade/install
	$(INSTALL_DIR) $(1)/usr/libexec
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/udx-boot-state $(1)/usr/libexec/
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/udx-image-check $(1)/usr/libexec/
	$(CP) ./files/. $(1)/
endef
$(eval $(call BuildPackage,udx-upgrade))


include $(TOPDIR)/rules.mk


PKG_NAME:=android-tools
PKG_VERSION:=4.2.2

PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=https://github.com/lkiuyu/android-tools.git
PKG_SOURCE_SUBDIR:=$(PKG_NAME)-$(PKG_VERSION)
PKG_SOURCE_VERSION:=6cea387346c9a49e0d7acbbe74f1cec62fcaec67
PKG_SOURCE:=$(PKG_NAME)-$(PKG_VERSION)-$(PKG_SOURCE_VERSION).tar.gz
PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_SOURCE_SUBDIR)
PKG_MAINTAINER:=HandsomeYingyan <handsomeyingyan@gmail.com>

include $(INCLUDE_DIR)/package.mk


define Package/android-tools/default
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=Android Debug Tools
  URL:=http://tools.android.com/
  DEPENDS:=+zlib +libopenssl +libpthread
endef

define Package/$(PKG_NAME)/Default/description
 Android Debug Bridge (adb) is a versatile command line tool that lets you communicate with an emulator instance or connected Android-powered device.
endef


define Package/$(PKG_NAME)-adbd
$(call Package/$(PKG_NAME)/default)
	TITLE+= - adbd
	DEPENDS:=+zlib +libopenssl +libpthread
endef

define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR) $(TARGET_CONFIGURE_OPTS) TARGET=Linux CFLAGS="$(TARGET_CFLAGS) $(TARGET_CPPFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)" -f $(PKG_BUILD_DIR)/adbd.mk
endef

define Package/$(PKG_NAME)-adbd/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/adbd $(1)/usr/bin/
	$(INSTALL_BIN) ./files/adbd.init $(1)/etc/init.d/adbd
	$(INSTALL_DATA) ./files/adb_profile $(1)/etc/
	$(INSTALL_DATA) ./files/adb_disable $(1)/etc/uci-defaults
endef

$(eval $(call BuildPackage,$(PKG_NAME)-adbd))

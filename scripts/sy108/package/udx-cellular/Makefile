include $(TOPDIR)/rules.mk

PKG_NAME:=udx-cellular
PKG_VERSION:=3.1
PKG_RELEASE:=8
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Package/udx-cellular
  SECTION:=net
  CATEGORY:=Network
  TITLE:=UDX710 native cellular and SMS owner
  DEPENDS:=+udx-cpnv +netifd +ubus +uci +procd +odhcp6c +libjson-c +libopenssl +libpthread +libuci +libubus +libubox +libblobmsg-json
endef

define Package/udx-cellular/conffiles
/etc/config/udx-cellular
/etc/udx-cellular/
endef

define Build/Compile
	CC="$(TARGET_CC)" CFLAGS="$(TARGET_CFLAGS)" CPPFLAGS="$(TARGET_CPPFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)" JSON_INCLUDE="$(STAGING_DIR)/usr/include" sh $(CURDIR)/build-native.sh $(PKG_BUILD_DIR)/bin -ljson-c
endef

define Package/udx-cellular/install
	$(INSTALL_DIR) $(1)
	$(CP) ./files/. $(1)/
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/usr/libexec
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/bin/udx-cellulard $(PKG_BUILD_DIR)/bin/udx-cellular-control $(PKG_BUILD_DIR)/bin/udx-system-quiesce $(1)/usr/sbin/
endef
$(eval $(call BuildPackage,udx-cellular))

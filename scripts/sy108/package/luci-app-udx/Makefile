include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-udx
PKG_VERSION:=3.1
PKG_RELEASE:=7
PKG_LICENSE:=GPL-2.0-only
PKGARCH:=all
PKG_BUILD_DEPENDS:=luci-base/host

include $(INCLUDE_DIR)/package.mk

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Package/luci-app-udx
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=3. Applications
  TITLE:=UDX710 device management
  DEPENDS:=+udx-device +luci-base +luci-mod-system +luci-i18n-base-zh-cn +luci-app-sy108-system
endef

define Build/Compile
	po2lmo $(CURDIR)/po/zh_Hans/udx.po $(PKG_BUILD_DIR)/udx.zh-cn.lmo
endef

define Package/luci-app-udx/install
	$(INSTALL_DIR) $(1)
	$(CP) ./root/. $(1)/
	$(INSTALL_DIR) $(1)/usr/lib/lua/luci/i18n
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/udx.zh-cn.lmo $(1)/usr/lib/lua/luci/i18n/
endef
$(eval $(call BuildPackage,luci-app-udx))

include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-sy108-system
PKG_VERSION:=4.0
PKG_RELEASE:=2
PKG_LICENSE:=GPL-2.0-only Apache-2.0
PKGARCH:=all
PKG_BUILD_DEPENDS:=luci-base/host

include $(INCLUDE_DIR)/package.mk

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Package/luci-app-sy108-system
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=3. Applications
  TITLE:=SY108 system maintenance
  DEPENDS:=+udx-upgrade +luci-base +luci-mod-system +luci-i18n-base-zh-cn
endef

define Build/Compile
	po2lmo $(CURDIR)/po/zh_Hans/udx.po $(PKG_BUILD_DIR)/sy108-system.zh-cn.lmo
endef

define Package/luci-app-sy108-system/install
	$(INSTALL_DIR) $(1)
	$(CP) ./root/. $(1)/
	$(INSTALL_DIR) $(1)/usr/lib/lua/luci/i18n
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/sy108-system.zh-cn.lmo $(1)/usr/lib/lua/luci/i18n/
endef
$(eval $(call BuildPackage,luci-app-sy108-system))

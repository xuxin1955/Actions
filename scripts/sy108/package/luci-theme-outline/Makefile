include $(TOPDIR)/rules.mk

PKG_NAME:=luci-theme-outline
PKG_VERSION:=1.1.0
PKG_RELEASE:=1
PKG_LICENSE:=Apache-2.0

include $(INCLUDE_DIR)/package.mk

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Package/luci-theme-outline
  SECTION:=luci
  CATEGORY:=LuCI
  TITLE:=luci-theme-outline for LuCI
  DEPENDS:=+luci-base +rpcd-mod-ucode +ucode-mod-fs +ucode-mod-uci +uclient-fetch +jsonfilter
endef

define Build/Compile
endef

define Package/luci-theme-outline/install
	$(CP) ./root/. $(1)/
endef

$(eval $(call BuildPackage,luci-theme-outline))

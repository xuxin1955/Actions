include $(TOPDIR)/rules.mk

PKG_NAME:=sy108-image
PKG_VERSION:=1.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
PKG_HOST_ONLY:=1

HOST_BUILD_DIR:=$(BUILD_DIR_HOST)/$(PKG_NAME)-$(PKG_VERSION)
include $(INCLUDE_DIR)/package.mk

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef
include $(INCLUDE_DIR)/host-build.mk

define Host/Prepare
	mkdir -p $(HOST_BUILD_DIR)
endef

define Package/sy108-image
  SECTION:=devel
  CATEGORY:=Development
  TITLE:=SY108 boot capsule host tools
  BUILDONLY:=1
endef

define Host/Compile
endef

define Host/Install
	rm -rf $(STAGING_DIR_HOST)/share/sy108-image
	$(INSTALL_DIR) $(STAGING_DIR_HOST)/share/sy108-image/tools/image
	$(INSTALL_DIR) $(STAGING_DIR_HOST)/share/sy108-image/tools/lib
	$(INSTALL_DATA) $(CURDIR)/src/tools/image/capsule.py $(STAGING_DIR_HOST)/share/sy108-image/tools/image/
	$(INSTALL_DATA) $(CURDIR)/src/tools/lib/boot_layout.py $(CURDIR)/src/tools/lib/memory_layout.py $(CURDIR)/src/tools/lib/newc.py $(STAGING_DIR_HOST)/share/sy108-image/tools/lib/
endef

define Host/Clean
	rm -rf $(STAGING_DIR_HOST)/share/sy108-image
endef

$(eval $(call BuildPackage,sy108-image))
$(eval $(call HostBuild))

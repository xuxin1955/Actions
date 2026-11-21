include $(TOPDIR)/rules.mk

PKG_NAME:=udx-usb
PKG_VERSION:=1.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
PKGARCH:=all

include $(INCLUDE_DIR)/package.mk

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Package/udx-usb
  SECTION:=net
  CATEGORY:=Network
  TITLE:=UDX fixed USB management gadget
  DEPENDS:=+netifd +procd +uci +ubus +jsonfilter +flock +dnsmasq +firewall4 +busybox +agetty +@BUSYBOX_CUSTOM +@BUSYBOX_CONFIG_LOGIN +@BUSYBOX_CONFIG_TASKSET
endef

define Package/udx-usb/description
  NCM hardware and software management modes with an optional
  password-authenticated ACM console. Requires the platform kernel's configfs,
  libcomposite, NCM, PAM U3 and ACM support; does not control USB power/role.
endef

define Package/udx-usb/conffiles
/etc/config/udx-usb
endef

define Build/Compile
endef

define Package/udx-usb/install
	$(INSTALL_DIR) $(1)
	$(CP) ./files/. $(1)/
endef

$(eval $(call BuildPackage,udx-usb))

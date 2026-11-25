include $(TOPDIR)/rules.mk

PKG_NAME:=udx-cpnv
PKG_VERSION:=4.0
PKG_RELEASE:=7
PKG_LICENSE:=GPL-2.0-only MIT
PKG_BUILD_DEPENDS:=openssl liblz4

include $(INCLUDE_DIR)/package.mk

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Package/udx-cpnv
  SECTION:=net
  CATEGORY:=Network
  TITLE:=UDX710 original-NAND CP and NV service
  DEPENDS:=+procd +libopenssl +liblz4 +libubus +libubox
endef

define Package/udx-cpnv/conffiles
/etc/udx-cpnv-warm/profile
endef

define Build/Compile
	$(INSTALL_DIR) $(PKG_BUILD_DIR)/bin
	$(TARGET_CC) $(TARGET_CPPFLAGS) $(TARGET_CFLAGS) -D_GNU_SOURCE -std=c11 -Wall -Wextra -Werror -I$(CURDIR)/src/cpnv/include $(CURDIR)/src/cpnv/src/*.c $(TARGET_LDFLAGS) -lubus -lubox -lcrypto -llz4 -pthread -o $(PKG_BUILD_DIR)/bin/udx-cpnvd
endef

define Package/udx-cpnv/install
	$(INSTALL_DIR) $(1)/lib/firmware/sprd
	$(LN) /run/udx-cpnv-warm $(1)/lib/firmware/sprd/udx710-cp
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/etc/udx-cpnv-warm
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/bin/udx-cpnvd $(1)/usr/sbin/
	$(CP) ./files/. $(1)/
	$(INSTALL_CONF) $(CURDIR)/src/cpnv/halo/halo-private.bin $(CURDIR)/src/cpnv/halo/halo-verification.bin $(1)/etc/udx-cpnv-warm/
	chmod 600 $(1)/etc/udx-cpnv-warm/profile $(1)/etc/udx-cpnv-warm/halo-private.bin $(1)/etc/udx-cpnv-warm/halo-verification.bin $(1)/etc/udx-refnotify/owner-policy
endef
$(eval $(call BuildPackage,udx-cpnv))

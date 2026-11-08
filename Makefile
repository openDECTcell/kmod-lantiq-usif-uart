# SPDX-License-Identifier: GPL-2.0-only
#
# OpenWrt package for the Lantiq VR9 USIF UART driver.
#
# Drop this directory into an OpenWrt feed or build it with the SDK
# (tools/build.sh); it produces lantiq_usif_uart.ko and /dev/ttyUSIF0.
#
include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=lantiq-usif-uart
PKG_VERSION:=0.1
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-or-later GPL-2.0-only
PKG_BUILD_DIR:=$(KERNEL_BUILD_DIR)/$(PKG_NAME)-$(PKG_VERSION)

USIF_UART_SRC:=$(abspath $(CURDIR)/src)

include $(INCLUDE_DIR)/package.mk

define KernelPackage/lantiq-usif-uart
  SUBMENU:=Other modules
  TITLE:=Lantiq VR9 USIF UART (ttyUSIF)
  DEPENDS:=@TARGET_lantiq_xrx200
  FILES:=$(PKG_BUILD_DIR)/lantiq_usif_uart.ko
  # No AUTOLOAD: the user (openDECTcell's service) loads it when needed.
endef

define KernelPackage/lantiq-usif-uart/description
  Serial driver for the Lantiq VR9 USIF in UART mode, one port as
  /dev/ttyUSIF0. On the AVM FRITZ!Box 7362 SL it instantiates itself on a
  stock image and uses GPIO 11 (RX) and GPIO 12 (TX).
endef

define Build/Prepare
	$(INSTALL_DIR) $(PKG_BUILD_DIR)
	$(CP) $(USIF_UART_SRC)/*.c $(USIF_UART_SRC)/*.h \
		$(USIF_UART_SRC)/Makefile $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" modules
endef

$(eval $(call KernelPackage,lantiq-usif-uart))

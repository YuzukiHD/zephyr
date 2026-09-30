# Copyright (c) 2026 Yuzuki Tsuru
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_include_directories(.)
zephyr_library_sources(usb_glue_cache.c)
zephyr_library_sources_ifdef(CONFIG_CHERRYUSB_DEVICE usb_glue_sunxi.c)

if(CONFIG_CHERRYUSB_HOST_EHCI_CUSTOM)
  zephyr_include_directories(
    ${ZEPHYR_CHERRYUSB_MODULE_DIR}/port/ehci
    ${ZEPHYR_CHERRYUSB_MODULE_DIR}/port/ohci
  )
  zephyr_library_sources(usb_glue_sunxi_host.c
    usb_hc_ohci.c)
endif()

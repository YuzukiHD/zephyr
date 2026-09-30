# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources(g2d_common.c)
zephyr_library_sources_ifdef(CONFIG_G2D_SHELL g2d_shell.c)

if(CONFIG_G2D_SUNXI)
  zephyr_library_sources(
    g2d_sunxi.c
    sunxi/g2d_sunxi_cmdlist.c
    sunxi/g2d_sunxi_compose.c
    sunxi/g2d_sunxi_format.c
    sunxi/g2d_sunxi_tables.c
  )
endif()

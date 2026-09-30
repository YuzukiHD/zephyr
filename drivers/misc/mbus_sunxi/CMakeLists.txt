# SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later

zephyr_library()
zephyr_library_sources(mbus_sunxi.c)
zephyr_library_sources_ifdef(CONFIG_MBUS_SUNXI_SHELL mbus_sunxi_shell.c)

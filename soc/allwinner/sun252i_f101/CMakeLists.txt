# Copyright (c) 2026 Yuzuki Tsuru
# SPDX-License-Identifier: Apache-2.0

zephyr_include_directories(.)

zephyr_sources(soc.c cache.c)
zephyr_sources_ifdef(CONFIG_SUN252I_F101_RVV soc_irq.S)

set(SOC_LINKER_SCRIPT ${ZEPHYR_BASE}/include/zephyr/arch/riscv/common/linker.ld CACHE INTERNAL "")

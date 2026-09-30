/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later */
/*
 * Display engine descriptors of the supported SoCs. Each one is the whole
 * pipeline layout of that SoC (channels, stages, offsets), so the driver
 * matches them by SoC specific compatible only, see de_drv.c.
 */
#ifndef __DE_SOC_H__
#define __DE_SOC_H__

#include "de_hw.h"

#ifdef CONFIG_DISPLAY_SOC_SUN252IW2
extern const struct de_soc_desc de_sun252iw2_desc;
#endif

#endif /* __DE_SOC_H__ */

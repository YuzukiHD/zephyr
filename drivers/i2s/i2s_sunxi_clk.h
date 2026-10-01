/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * PLL_AUDIO1 and the audio module clocks, shared by the codec, I2S0 and OWA.
 *
 * Two sample rate families exist, each needs its own PLL setting:
 *   48 kHz family   (8000, 12000, 16000, 24000, 32000, 48000, 96000, 192000)
 *     PLL 3.072 GHz, PLL/5 = 614.4 MHz feeds the module clocks
 *   44.1 kHz family (11025, 22050, 44100, 88200, 176400)
 *     PLL 2.1676 GHz (fractional), PLL/2 = 1083.8 MHz feeds the module clocks
 * The PLL is shared, so all users have to be in the same family at a time.
 */

#ifndef ZEPHYR_DRIVERS_I2S_I2S_SUNXI_CLK_H_
#define ZEPHYR_DRIVERS_I2S_I2S_SUNXI_CLK_H_

#include <stdint.h>

enum sunxi_audio_family {
	SUNXI_AUDIO_FAMILY_48K,
	SUNXI_AUDIO_FAMILY_44K1,
};

/* Family of a frame rate, or -EINVAL when it belongs to neither */
int sunxi_audio_family(uint32_t frame_rate);

/* Master clock the family is designed around: 24.576 MHz or 22.5792 MHz */
uint32_t sunxi_audio_base_mclk(enum sunxi_audio_family family);

/*
 * Take a reference on the PLL in the given family and start it when it was off.
 * Returns -EBUSY when it runs in the other family for someone else.
 */
int sunxi_audio_pll_get(enum sunxi_audio_family family);
void sunxi_audio_pll_put(void);

/*
 * Program an audio module clock register (source select, divider, gate) to
 * produce @p rate from the running PLL and open its gate. Returns the rate
 * obtained in @p actual when not NULL, -ERANGE when it cannot be made within
 * 1 percent.
 */
int sunxi_audio_module_clk_set(uint32_t reg, uint32_t rate, uint32_t *actual);
void sunxi_audio_module_clk_off(uint32_t reg);

#endif /* ZEPHYR_DRIVERS_I2S_I2S_SUNXI_CLK_H_ */

/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Decodes an embedded ADTS clip with the Helix AAC decoder and compares it with a reference decode */

#include <stdint.h>
#include <stdlib.h>
#include <aacdec.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "testdata.h"

static int16_t all[60000];

int main(void)
{
	HAACDecoder h = AACInitDecoder();
	AACFrameInfo fi;
	unsigned char *p = (unsigned char *)aac_data;
	int left = sizeof(aac_data), frames = 0, ret;
	int64_t cycles = 0;
	size_t out = 0;

	printk("open %p\n", h);
	while (left > 0) {
		int off = AACFindSyncWord(p, left);
		uint32_t t0;

		if (off < 0) {
			break;
		}
		p += off;
		left -= off;
		t0 = k_cycle_get_32();
		ret = AACDecode(h, &p, &left, &all[out]);
		cycles += k_cycle_get_32() - t0;
		if (ret != 0) {
			printk("decode error %d at frame %d\n", ret, frames);
			break;
		}
		AACGetLastFrameInfo(h, &fi);
		if (frames == 0) {
			printk("%d Hz, %d ch, %d bit, %d samples\n", fi.sampRateCore, fi.nChans,
			       fi.bitsPerSample, fi.outputSamps);
		}
		out += fi.outputSamps;
		frames++;
		if (out + 2048 > ARRAY_SIZE(all)) {
			break;
		}
	}
	printk("%d frames, %lld us (%lld us/frame), %zu samples\n", frames,
	       k_cyc_to_us_near64(cycles), frames ? k_cyc_to_us_near64(cycles) / frames : 0, out);
	{
		int best = 0;
		int64_t best_err = INT64_MAX, err2 = 0, sig2 = 0;
		int max = 0;

		for (int sh = -3072; sh <= 3072; sh += 2) {
			int64_t e = 0;

			for (int i = 4096; i < 40000; i += 7) {
				int d = all[i] - ref_pcm[i + sh];

				e += (int64_t)d * d;
			}
			if (e < best_err) {
				best_err = e;
				best = sh;
			}
		}
		for (int i = 4096; i < 40000; i++) {
			int d = all[i] - ref_pcm[i + best];

			err2 += (int64_t)d * d;
			sig2 += (int64_t)ref_pcm[i + best] * ref_pcm[i + best];
			max = MAX(max, abs(d));
		}
		printk("best shift %d, max diff %d, error energy %lld of %lld\n", best, max, err2,
		       sig2);
		printk("aac test %s\n", max <= 4 ? "PASS" : "FAIL");
	}
	AACFreeDecoder(h);
	k_sleep(K_FOREVER);

	return 0;
}

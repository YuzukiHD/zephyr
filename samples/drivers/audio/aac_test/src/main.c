/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Decodes an embedded ADTS clip with the Helix AAC decoder and compares it with a reference decode */

#include <stdint.h>
#include <stdlib.h>
#include <pvmp4audiodecoder_api.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "testdata.h"

static int16_t all[60000 + 4096];

/* AudioSpecificConfig of AAC-LC, 48 kHz, stereo */
static const uint8_t asc[2] = {0x11, 0x90};

int main(void)
{
	tPVMP4AudioDecoderExternal ext = {0};
	void *mem = malloc(PVMP4AudioDecoderGetMemRequirements());
	int ret, frames = 0;
	int64_t cycles = 0;
	size_t out = 0;
	const uint8_t *p = aac_data;
	size_t left = sizeof(aac_data);

	ext.desiredChannels = 2;
	ext.outputFormat = OUTPUTFORMAT_16PCM_INTERLEAVED;
	ext.aacPlusEnabled = false;
	ret = PVMP4AudioDecoderInitLibrary(&ext, mem);
	printk("init %d, memory %u bytes\n", ret, PVMP4AudioDecoderGetMemRequirements());
	ext.pInputBuffer = (UChar *)asc;
	ext.inputBufferCurrentLength = sizeof(asc);
	ext.inputBufferUsedLength = 0;
	ext.remainderBits = 0;
	ret = PVMP4AudioDecoderConfig(&ext, mem);
	printk("config %d: %d Hz, %d ch, object %d\n", ret, (int)ext.samplingRate,
	       ext.encodedChannels, ext.audioObjectType);

	/* the clip is ADTS: cut the frames out by the 13 bit length of the header */
	while (left > 7 && out + 2048 <= ARRAY_SIZE(all)) {
		size_t flen = ((p[3] & 3) << 11) | (p[4] << 3) | (p[5] >> 5);
		size_t hdr = (p[1] & 1) ? 7 : 9;
		uint32_t t0;

		if (p[0] != 0xff || (p[1] & 0xf0) != 0xf0 || flen < hdr || flen > left) {
			break;
		}
		ext.pInputBuffer = (UChar *)p + hdr;
		ext.inputBufferCurrentLength = flen - hdr;
		ext.inputBufferUsedLength = 0;
		ext.remainderBits = 0;
		ext.pOutputBuffer = &all[out];
		ext.pOutputBuffer_plus = &all[out] + 2048;
		t0 = k_cycle_get_32();
		ret = PVMP4AudioDecodeFrame(&ext, mem);
		cycles += k_cycle_get_32() - t0;
		if (ret != MP4AUDEC_SUCCESS) {
			printk("decode error %d at frame %d\n", ret, frames);
			break;
		}
		if (frames == 0) {
			printk("frame: %d samples/ch, %d ch\n", ext.frameLength, ext.desiredChannels);
		}
		out += ext.frameLength * ext.desiredChannels;
		p += flen;
		left -= flen;
		frames++;
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
		printk("aac test %s\n", max <= 8 ? "PASS" : "FAIL");
	}
	free(mem);
	k_sleep(K_FOREVER);

	return 0;
}

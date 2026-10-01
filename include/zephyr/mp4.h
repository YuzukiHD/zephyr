/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief MP4 / ISO base media file demultiplexer
 *
 * Reads the sample tables of the first H.264 and the first AAC track of a file
 * into memory and walks the samples of a track in decoding order. The file is
 * accessed through a read callback, nothing else is needed from the file system.
 */

#ifndef ZEPHYR_INCLUDE_MP4_H_
#define ZEPHYR_INCLUDE_MP4_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum mp4_codec {
	MP4_CODEC_NONE,
	MP4_CODEC_H264,	/**< extra data is the avcC record */
	MP4_CODEC_AAC,	/**< extra data is the AudioSpecificConfig */
};

/** Reads @p len bytes at @p offset, returns 0 or a negative errno */
typedef int (*mp4_read_t)(void *ctx, uint64_t offset, void *buf, size_t len);

struct mp4_sample {
	uint64_t offset;	/**< position in the file */
	uint32_t size;
	int64_t dts;		/**< decoding time, track timescale units */
	int64_t pts;		/**< presentation time, track timescale units */
	uint32_t index;
	uint8_t sync;		/**< key frame (always set for audio) */
};

struct mp4_track {
	enum mp4_codec codec;
	uint32_t timescale;
	uint64_t duration;	/**< track timescale units */
	int64_t media_start;	/**< presentation starts at this media time (edit list), 0 if none */
	uint32_t sample_count;
	uint32_t max_sample;	/**< size of the largest sample */
	uint16_t width, height;	/**< video */
	uint16_t channels;	/**< audio */
	uint32_t sample_rate;	/**< audio, Hz */
	uint8_t *extra;
	uint32_t extra_len;

	/* sample tables */
	uint32_t *sizes;
	uint32_t fixed_size;
	uint32_t chunk_count;
	uint32_t *chunk_offset;
	uint32_t stsc_count;
	uint32_t *stsc;		/* first_chunk, samples_per_chunk, pairs */
	uint32_t stts_count;
	uint32_t *stts;		/* count, delta, pairs */
	uint32_t ctts_count;
	uint32_t *ctts;		/* count, offset, pairs */
	uint32_t stss_count;
	uint32_t *stss;		/* 1-based sync sample numbers, NULL when all are sync */
};

/** Cursor over the samples of a track, start from zero-initialised memory */
struct mp4_iter {
	uint32_t next;		/**< index of the next sample */
	uint32_t chunk;		/**< chunk of the next sample */
	uint32_t in_chunk;	/**< samples of that chunk already passed */
	uint32_t stsc_idx;
	uint64_t chunk_pos;	/**< file offset of the next sample */
	uint32_t stts_idx, stts_left;
	uint32_t ctts_idx, ctts_left;
	int64_t dts;
	uint32_t stss_idx;
};

struct mp4 {
	mp4_read_t read;
	void *ctx;
	uint64_t size;
	struct mp4_track video;
	struct mp4_track audio;
};

/** Parses the movie box; tracks that are not H.264 / AAC are ignored */
int mp4_open(struct mp4 *m, mp4_read_t read, void *ctx, uint64_t file_size);
void mp4_close(struct mp4 *m);

/** Starts an iterator at the beginning of a track */
void mp4_iter_init(const struct mp4_track *t, struct mp4_iter *it);

/** Next sample in decoding order; -ENODATA at the end */
int mp4_next(const struct mp4_track *t, struct mp4_iter *it, struct mp4_sample *s);

/** Positions the iterator on the last sync sample at or before @p index */
int mp4_seek_sync(const struct mp4_track *t, struct mp4_iter *it, uint32_t index);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_MP4_H_ */

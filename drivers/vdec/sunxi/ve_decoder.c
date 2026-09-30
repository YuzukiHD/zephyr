/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "glue/glue_mem.h"
#include "ve_abi.h"
#include "ve_decoder.h"
#include "ve_log.h"

VE_LOG_DECLARE();

#define MAX_STREAM_WIDTH	6144
#define MAX_STREAM_HEIGHT	3840
#define MAX_STREAMS		2

/* A JPEG start/end marker is searched in at most this many bytes of a part */
#define JPEG_MARKER_SCAN	512

struct ve_decoder {
	struct ve_vconfig vconfig;
	struct ve_stream_info stream_info;
	struct ve_engine *engine;
	int fbm_num;
	struct ve_fbm *fbm[MAX_STREAMS];
	int sbm_num;
	struct ve_sbm *sbm[MAX_STREAMS];
	/* the picture being assembled from parts submitted so far, per stream */
	struct ve_stream_data partial[MAX_STREAMS];
	struct ve_mem_ops *memops;
};

/*
 * The codec plugins register themselves with the engine layer when asked to.
 * Destroying an engine empties its registry, so this is done for every one.
 */
static void register_plugins(void)
{
	if (IS_ENABLED(CONFIG_VDEC_SUNXI_JPEG)) {
		CedarPluginVDInit_mjpeg();
	}
	if (IS_ENABLED(CONFIG_VDEC_SUNXI_PNG)) {
		CedarPluginVDInit_png();
	}
	if (IS_ENABLED(CONFIG_VDEC_SUNXI_H264)) {
		CedarPluginVDInit_h264();
	}
}

struct ve_decoder *ve_decoder_create(void)
{
	struct ve_decoder *dec = calloc(1, sizeof(*dec));

	if (dec == NULL) {
		return NULL;
	}
	for (int i = 0; i < MAX_STREAMS; i++) {
		dec->partial[i].stream_index = i;
		dec->partial[i].pts = -1;
	}

	return dec;
}

void ve_decoder_destroy(struct ve_decoder *dec)
{
	if (dec == NULL) {
		return;
	}
	/* the engine takes the stream buffers down with it */
	if (dec->engine != NULL) {
		VideoEngineDestroy(dec->engine);
	}
	free(dec->stream_info.csd);
	if (dec->memops != NULL) {
		dec->memops->close();
	}
	free(dec);
}

static int stream_buffer_size(const struct ve_decoder *dec)
{
	int size = CONFIG_VDEC_SUNXI_STREAM_BUFFER_KB * 1024;
	int want = dec->vconfig.vbv_buffer_size;

	/* a caller that knows the size of its data can ask for more room */
	if (want > size) {
		size = want;
	}

	return size;
}

int ve_decoder_init(struct ve_decoder *dec, const struct ve_stream_info *info,
		    const struct ve_vconfig *cfg)
{
	struct ve_vconfig vc = *cfg;
	struct ve_sbm_config sc = {0};
	int sbm_size;

	if (info->codec_format < VE_CODEC_MIN || info->codec_format > VE_CODEC_MAX) {
		LOG_ERR("invalid codec format 0x%x", info->codec_format);
		return -1;
	}
	if (info->height > MAX_STREAM_HEIGHT || info->width > MAX_STREAM_WIDTH) {
		LOG_ERR("%dx%d is not supported", info->width, info->height);
		return VE_RESULT_UNSUPPORTED;
	}

	/* the JPEG decoder writes 16 pixel aligned rows only */
	if (info->codec_format == VE_CODEC_MJPEG && vc.align_stride > 16) {
		int width = info->width;

		if (vc.scale_down_en == 1 && vc.horizon_scale_down_ratio != 0) {
			width >>= vc.horizon_scale_down_ratio;
		}
		if (width > 0 && (width % vc.align_stride) != 0) {
			LOG_WRN("stride alignment %d lowered to 16 for this width", vc.align_stride);
			vc.align_stride = 16;
		}
	}
	/* the engine cannot handle very large pictures at full size */
	if (info->height >= 3840 || info->width >= 5120) {
		vc.scale_down_en = 1;
		vc.horizon_scale_down_ratio = 1;
		vc.vertical_scale_down_ratio = 1;
	}

	dec->memops = ve_mem_get_ops();
	vc.memops = dec->memops;
	if (dec->memops->open() < 0) {
		return -1;
	}

	dec->stream_info.codec_format = info->codec_format;
	dec->stream_info.width = info->width;
	dec->stream_info.height = info->height;
	dec->stream_info.frame_rate = info->frame_rate;
	dec->stream_info.frame_duration = info->frame_duration;
	dec->stream_info.aspect_ratio = info->aspect_ratio;
	dec->stream_info.is_3d_stream = info->is_3d_stream;
	dec->stream_info.is_raw_stream = info->is_raw_stream;
	dec->stream_info.is_frame_package = info->is_frame_package;
	dec->stream_info.csd_len = info->csd_len;
	dec->stream_info.secure_stream = info->secure_stream;
	dec->stream_info.secure_stream_level1 = info->secure_stream_level1;
	dec->stream_info.is_frame_cts_test = info->is_frame_cts_test;
	if (info->csd_len > 0) {
		dec->stream_info.csd = malloc(info->csd_len);
		if (dec->stream_info.csd == NULL) {
			dec->stream_info.csd_len = 0;
			return -1;
		}
		memcpy(dec->stream_info.csd, info->csd, info->csd_len);
	}

	dec->vconfig = vc;
	sbm_size = stream_buffer_size(dec);
	dec->vconfig.max_memory_available = CONFIG_VDEC_SUNXI_MAX_MEMORY_KB * 1024 - sbm_size;
	if (dec->vconfig.max_memory_available < 0) {
		LOG_ERR("not enough memory to decode");
		return -1;
	}

	register_plugins();
	dec->engine = VideoEngineCreate(&dec->vconfig, &dec->stream_info);
	if (dec->engine == NULL) {
		LOG_ERR("cannot create the video engine");
		return -1;
	}

	/* the engine fills in the access table it wants the buffers allocated with */
	sc.vir_flag = dec->vconfig.vir_malloc_sbm;
	sc.memops = dec->memops;
	sc.sbm_buffer_total_size = sbm_size;
	sc.ve_ops = dec->vconfig.ve_ops;
	sc.ve_ops_self = dec->vconfig.ve_ops_self;
	sc.secure_video = dec->vconfig.secure_os_en;
	sc.nalu_length = 4;

	for (int i = 0; i < (info->is_3d_stream ? 2 : 1); i++) {
		dec->sbm[i] = ve_sbm_create(VE_SBM_STREAM);
		if (dec->sbm[i] == NULL) {
			return -1;
		}
		if (dec->sbm[i]->init(dec->sbm[i], &sc) != 0) {
			LOG_ERR("cannot set up the stream buffer");
			free(dec->sbm[i]);
			dec->sbm[i] = NULL;
			return -1;
		}
		VideoEngineSetSbm(dec->engine, dec->sbm[i], i);
		dec->sbm_num++;
	}

	return 0;
}

void ve_decoder_reset(struct ve_decoder *dec)
{
	if (dec->engine != NULL) {
		VideoEngineReset(dec->engine);
		if (dec->fbm_num == 0) {
			dec->fbm_num = VideoEngineGetFbmNum(dec->engine);
		}
	}
	for (int i = 0; i < dec->fbm_num; i++) {
		if (dec->fbm[i] == NULL && dec->engine != NULL) {
			dec->fbm[i] = VideoEngineGetFbm(dec->engine, i);
		}
		if (dec->fbm[i] != NULL) {
			FbmFlush(dec->fbm[i]);
		}
	}
	for (int i = 0; i < dec->sbm_num; i++) {
		if (dec->sbm[i] != NULL) {
			dec->sbm[i]->reset(dec->sbm[i]);
		}
	}
	memset(dec->partial, 0, sizeof(dec->partial));
	for (int i = 0; i < MAX_STREAMS; i++) {
		dec->partial[i].stream_index = i;
		dec->partial[i].pts = -1;
	}
}

int ve_decoder_reopen(struct ve_decoder *dec, struct ve_vconfig *cfg, struct ve_stream_info *info)
{
	if (dec->engine == NULL) {
		return -1;
	}
	free(dec->stream_info.csd);
	dec->stream_info.csd = NULL;

	if (VideoEngineReopen(dec->engine, cfg, info) < 0) {
		LOG_ERR("cannot reopen the video engine");
		return -1;
	}
	dec->fbm[0] = dec->fbm[1] = NULL;
	dec->fbm_num = 0;
	dec->vconfig = *cfg;
	dec->stream_info = *info;
	info->reopen_engine = 1;
	dec->vconfig.ve_ops_self = dec->engine->ve_ops_self;
	dec->vconfig.ve_ops = dec->engine->ve_ops;
	dec->stream_info.csd = NULL;
	dec->stream_info.csd_len = 0;
	if (info->csd != NULL && info->csd_len > 0) {
		dec->stream_info.csd = malloc(info->csd_len);
		if (dec->stream_info.csd == NULL) {
			return -1;
		}
		memcpy(dec->stream_info.csd, info->csd, info->csd_len);
		dec->stream_info.csd_len = info->csd_len;
	}
	for (int i = 0; i < dec->sbm_num; i++) {
		VideoEngineSetSbm(dec->engine, dec->sbm[i], i);
	}

	return 0;
}

int ve_decoder_decode(struct ve_decoder *dec, int end_of_stream, int key_frame_only,
		      int drop_b_frame_if_delay, int64_t current_time_us)
{
	int ret;

	if (dec->engine == NULL) {
		return VE_RESULT_UNSUPPORTED;
	}

	if (end_of_stream == 1) {
		for (int i = 0; i < dec->sbm_num; i++) {
			dec->sbm[i]->set_eos(dec->sbm[i], end_of_stream);
		}
	}

	/*
	 * The engine writes tiled pictures and converts each one into a second,
	 * linear picture, so a decoding step needs two free pictures.
	 */
	if (dec->stream_info.codec_format != VE_CODEC_PNG) {
		struct ve_fbm *fbm = dec->fbm[0];

		if (fbm == NULL) {
			fbm = dec->fbm[0] = VideoEngineGetFbm(dec->engine, 0);
		}
		if (fbm != NULL && FbmEmptyBufferNum(fbm) < 2) {
			return VE_RESULT_NO_FRAME_BUFFER;
		}
	}

	ret = VideoEngineDecode(dec->engine, end_of_stream, key_frame_only, drop_b_frame_if_delay,
				current_time_us);

	if (ret == VE_RESULT_RESOLUTION_CHANGE) {
		/* the engine makes new frame buffer pools */
		dec->fbm[0] = dec->fbm[1] = NULL;
		dec->fbm_num = 0;
	}
	if (!dec->vconfig.thumbnail_mode && end_of_stream && ret == VE_RESULT_NO_BITSTREAM) {
		VideoEngineReset(dec->engine);
	}
	/* plenty of data queued but not enough for a picture: keep feeding */
	if (ret == VE_RESULT_NO_BITSTREAM && ve_decoder_stream_data_size(dec, 0) > 512 * 1024) {
		ret = VE_RESULT_CONTINUE;
	}

	return ret;
}

void ve_decoder_get_stream_info(struct ve_decoder *dec, struct ve_stream_info *info)
{
	info->codec_format = dec->stream_info.codec_format;
	info->width = dec->stream_info.width;
	info->height = dec->stream_info.height;
	info->frame_rate = dec->stream_info.frame_rate;
	info->frame_duration = dec->stream_info.frame_duration;
	info->aspect_ratio = dec->stream_info.aspect_ratio;
	info->is_3d_stream = dec->stream_info.is_3d_stream;
}

/* ---- compressed data in ---- */

int ve_decoder_request_stream_buffer(struct ve_decoder *dec, int size, char **buf, int *buf_size,
				     char **ring_buf, int *ring_buf_size, int stream)
{
	struct ve_sbm *sbm = stream < MAX_STREAMS ? dec->sbm[stream] : NULL;
	int already = dec->partial[stream].length;
	char *mem, *ring_end, *start;
	int room;

	*buf = *ring_buf = NULL;
	*buf_size = *ring_buf_size = 0;
	if (sbm == NULL) {
		LOG_WRN("stream %d has no stream buffer", stream);
		return -1;
	}

	if (size == 0) {
		size = 4;
	}
	size += already;

	if (dec->stream_info.codec_format == VE_CODEC_PNG) {
		/* a PNG is handed over as one block: the whole buffer is the room */
		room = size;
		mem = sbm->get_buffer_address(sbm);
		if (mem == NULL) {
			return -1;
		}
	} else if (sbm->request_buffer(sbm, size, &mem, &room) < 0) {
		LOG_DBG("stream buffer %d is full (%d bytes of %d)", stream,
			sbm->get_stream_data_size(sbm), sbm->get_buffer_size(sbm));
		return -1;
	}
	if (room <= already) {
		LOG_ERR("room of %d bytes is not larger than the %d bytes already written", room,
			already);
		return -1;
	}

	ring_end = (char *)sbm->get_buffer_address(sbm) + sbm->get_buffer_size(sbm);
	start = mem + already;
	if (start >= ring_end) {
		start -= sbm->get_buffer_size(sbm);
	}
	room -= already;

	if (start + room <= ring_end) {
		*buf = start;
		*buf_size = room;
	} else {
		*buf = start;
		*buf_size = ring_end - start;
		*ring_buf = sbm->get_buffer_address(sbm);
		*ring_buf_size = room - *buf_size;
	}

	return 0;
}

/* The bytes just written must be visible to the engine, which reads memory directly */
static void flush_stream_data(struct ve_decoder *dec, struct ve_sbm *sbm,
			      struct ve_stream_data *part)
{
	char *ring_end;

	if (dec->vconfig.vir_malloc_sbm != 0 || sbm->use_new_ve_memory_program != 0) {
		return;
	}
	ring_end = (char *)sbm->get_buffer_address(sbm) + sbm->get_buffer_size(sbm);
	if (part->data + part->length <= ring_end) {
		dec->memops->flush_cache(part->data, part->length);
	} else {
		int first = ring_end - part->data;

		dec->memops->flush_cache(part->data, first);
		dec->memops->flush_cache(sbm->get_buffer_address(sbm), part->length - first);
	}
}

/* Copy `len` bytes at `src`, which may wrap around the end of the ring, into `dst` */
static void read_ring(const struct ve_sbm *sbm, char *dst, const char *src, int len)
{
	const char *base = sbm->get_buffer_address((struct ve_sbm *)sbm);
	int size = sbm->get_buffer_size((struct ve_sbm *)sbm);
	const char *end = base + size;

	if (src + len <= end) {
		memcpy(dst, src, len);
	} else {
		int first = end - src;

		memcpy(dst, src, first);
		memcpy(dst + first, base, len - first);
	}
}

/*
 * A JPEG picture starts with the SOI marker (FFD8) and ends with EOI (FFD9).
 * The caller does not say where a picture begins or ends, so find out from
 * the data. Fill bytes (00/FF) in front of a marker are skipped.
 */
static void jpeg_mark_parts(struct ve_decoder *dec, struct ve_stream_data *data, int stream)
{
	struct ve_sbm *sbm = dec->sbm[stream];
	char buf[JPEG_MARKER_SCAN];
	int check = MIN(data->length, JPEG_MARKER_SCAN);
	uint16_t code = 0;
	int i;

	data->is_first_part = 0;
	data->is_last_part = 0;

	read_ring(sbm, buf, data->data, check);
	for (i = 0; i < check; i++) {
		code = (code << 8) | (uint8_t)buf[i];
		if (code == 0xFFD8) {
			data->is_first_part = 1;
			break;
		} else if (code != 0 && code != 0xFFFF && code != 0x00FF) {
			break;
		}
	}

	{
		const char *tail = data->data + data->length - check;
		const char *base = sbm->get_buffer_address(sbm);

		if (tail < base) {
			tail += sbm->get_buffer_size(sbm);
		} else if (tail >= base + sbm->get_buffer_size(sbm)) {
			tail -= sbm->get_buffer_size(sbm);
		}
		read_ring(sbm, buf, tail, check);
	}
	code = 0;
	for (i = check - 1; i >= 0; i--) {
		code = (code >> 8) | ((uint16_t)(uint8_t)buf[i] << 8);
		if (code == 0xFFD9) {
			data->is_last_part = 1;
			break;
		} else if (code != 0 && code != 0xFFFF && code != 0x00FF) {
			if (i > 0 && (((uint8_t)buf[i - 1] << 8) | (uint8_t)buf[i]) == 0xFFD9) {
				data->is_last_part = 1;
				break;
			}
		}
	}
}

int ve_decoder_submit_stream(struct ve_decoder *dec, struct ve_stream_data *data, int stream)
{
	struct ve_sbm *sbm = stream < MAX_STREAMS ? dec->sbm[stream] : NULL;
	struct ve_stream_data *part = &dec->partial[stream];

	if (sbm == NULL) {
		LOG_WRN("stream %d has no stream buffer", stream);
		return -1;
	}

	if (dec->stream_info.codec_format == VE_CODEC_MJPEG) {
		jpeg_mark_parts(dec, data, stream);
	}

	if (data->is_first_part) {
		/* the previous picture never got its last part: hand over what there is */
		if (part->length != 0) {
			flush_stream_data(dec, sbm, part);
			sbm->add_stream(sbm, part);
		}
		part->data = data->data;
		part->length = data->length;
		part->pts = data->pts;
		part->pcr = data->pcr;
		part->is_first_part = data->is_first_part;
		part->is_last_part = 0;
		part->video_info_flag = data->video_info_flag;
		part->video_info = data->video_info;
		part->valid = 1;
	} else {
		if (part->data == NULL) {
			part->data = data->data;
		}
		part->length += data->length;
		if (part->pts == -1 && data->pts != -1) {
			part->pts = data->pts;
		}
	}

	if (data->is_last_part) {
		if (part->data != NULL && part->length != 0) {
			flush_stream_data(dec, sbm, part);
		}
		sbm->add_stream(sbm, part);
		part->data = NULL;
		part->length = 0;
		part->pts = -1;
		part->pcr = -1;
		part->is_last_part = 0;
		part->is_first_part = 0;
		part->video_info_flag = 0;
		part->video_info = NULL;
		part->valid = 0;
	}

	return 0;
}

void *ve_decoder_stream_buffer_address(struct ve_decoder *dec, int stream)
{
	return dec->sbm[stream] != NULL ? dec->sbm[stream]->get_buffer_address(dec->sbm[stream]) :
					  NULL;
}

int ve_decoder_stream_buffer_size(struct ve_decoder *dec, int stream)
{
	return dec->sbm[stream] != NULL ? dec->sbm[stream]->get_buffer_size(dec->sbm[stream]) : 0;
}

int ve_decoder_stream_data_size(struct ve_decoder *dec, int stream)
{
	return dec->sbm[stream] != NULL ?
		       dec->sbm[stream]->get_stream_data_size(dec->sbm[stream]) : 0;
}

int ve_decoder_stream_frame_num(struct ve_decoder *dec, int stream)
{
	return dec->sbm[stream] != NULL ?
		       dec->sbm[stream]->get_stream_frame_num(dec->sbm[stream]) : 0;
}

/* ---- pictures out ---- */

static struct ve_fbm *fbm_of(struct ve_decoder *dec, int stream)
{
	if (stream < 0 || stream >= MAX_STREAMS) {
		return NULL;
	}
	if (dec->fbm[stream] == NULL && dec->engine != NULL) {
		dec->fbm[stream] = VideoEngineGetFbm(dec->engine, stream);
	}

	return dec->fbm[stream];
}

/* Keep the stream description in step with what the pictures really are */
static void update_stream_info(struct ve_decoder *dec, const struct ve_picture *pic)
{
	struct ve_stream_info *si = &dec->stream_info;
	const struct ve_vconfig *vc = &dec->vconfig;
	bool rotated = vc->rotation_en != 0 && vc->rotate_degree != 0 && vc->rotate_degree != 3;
	int w = rotated ? pic->height : pic->width;
	int h = rotated ? pic->width : pic->height;

	if (vc->scale_down_en) {
		w <<= vc->horizon_scale_down_ratio;
		h <<= vc->vertical_scale_down_ratio;
	}
	si->width = w;
	si->height = h;
	if (pic->frame_rate != 0) {
		si->frame_rate = pic->frame_rate;
	}
}

struct ve_picture *ve_decoder_request_picture(struct ve_decoder *dec, int stream)
{
	struct ve_fbm *fbm = fbm_of(dec, stream);
	struct ve_picture *pic;

	if (fbm == NULL) {
		return NULL;
	}
	pic = FbmRequestPicture(fbm);
	if (pic != NULL) {
		pic->stream_index = stream;
		update_stream_info(dec, pic);
	}

	return pic;
}

int ve_decoder_return_picture(struct ve_decoder *dec, struct ve_picture *pic)
{
	struct ve_fbm *fbm;

	if (pic == NULL) {
		return 0;
	}
	if (pic->stream_index < 0 || pic->stream_index >= MAX_STREAMS) {
		LOG_ERR("picture has an invalid stream index %d", pic->stream_index);
		return -1;
	}
	fbm = dec->fbm[pic->stream_index];
	if (fbm == NULL) {
		LOG_WRN("picture returned before any was handed out");
		return -1;
	}

	return FbmReturnPicture(fbm, pic);
}

struct ve_picture *ve_decoder_next_picture_info(struct ve_decoder *dec, int stream)
{
	struct ve_fbm *fbm = fbm_of(dec, stream);
	struct ve_picture *pic = fbm != NULL ? FbmNextPictureInfo(fbm) : NULL;

	if (pic != NULL) {
		pic->stream_index = stream;
		update_stream_info(dec, pic);
	}

	return pic;
}

int ve_decoder_total_picture_buffer_num(struct ve_decoder *dec, int stream)
{
	struct ve_fbm *fbm = fbm_of(dec, stream);

	return fbm != NULL ? FbmTotalBufferNum(fbm) : 0;
}

int ve_decoder_empty_picture_buffer_num(struct ve_decoder *dec, int stream)
{
	struct ve_fbm *fbm = fbm_of(dec, stream);

	return fbm != NULL ? FbmEmptyBufferNum(fbm) : 0;
}

int ve_decoder_valid_picture_num(struct ve_decoder *dec, int stream)
{
	struct ve_fbm *fbm = fbm_of(dec, stream);

	return fbm != NULL ? FbmValidPictureNum(fbm) : 0;
}

/* ---- settings ---- */

int ve_decoder_config_extra_scale(struct ve_decoder *dec, int width_th, int height_th,
				  int horizon_ratio, int vertical_ratio)
{
	struct ve_decoder_if *di = dec->engine != NULL ? dec->engine->decoder : NULL;

	if (di == NULL) {
		return VE_RESULT_UNSUPPORTED;
	}

	return di->set_extra_scale_info != NULL ?
		       di->set_extra_scale_info(di, width_th, height_th, horizon_ratio,
						vertical_ratio) : 0;
}

int ve_decoder_config_rotate(struct ve_decoder *dec, int degree)
{
	struct ve_decoder_if *di = dec->engine != NULL ? dec->engine->decoder : NULL;

	if (di == NULL) {
		return VE_RESULT_UNSUPPORTED;
	}

	return di->set_rotate_info != NULL ? di->set_rotate_info(di, degree) : 0;
}

int ve_decoder_set_freq(struct ve_decoder *dec, int mhz)
{
	struct ve_ops *ops = dec->vconfig.ve_ops;

	if (ops == NULL) {
		return -1;
	}

	return ops->set_speed(dec->vconfig.ve_ops_self, mhz);
}

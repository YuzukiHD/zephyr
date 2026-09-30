/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Data layouts shared with the prebuilt decoder archive.
 *
 * The archive reads and writes these structures directly, so member order,
 * types and sizes are fixed by it and must not be rearranged. Every struct is
 * covered by the layout check in tools/vdec_abi_check, which compares the
 * offsets of all members with what the archive was built against.
 */

#ifndef ZEPHYR_DRIVERS_VDEC_SUNXI_VE_ABI_H_
#define ZEPHYR_DRIVERS_VDEC_SUNXI_VE_ABI_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Codec ids */
enum ve_codec {
	VE_CODEC_UNKNOWN = 0,
	VE_CODEC_MJPEG = 0x101,
	VE_CODEC_H264 = 0x115,
	VE_CODEC_PNG = 0x119,
	VE_CODEC_MIN = 0x101,
	VE_CODEC_MAX = 0x119,
};

/* Pixel formats of decoded pictures */
enum ve_pixel_format {
	VE_PIX_DEFAULT = 0,
	VE_PIX_YUV_PLANAR_420 = 1,
	VE_PIX_YUV_PLANAR_422 = 2,
	VE_PIX_YUV_PLANAR_444 = 3,
	VE_PIX_YV12 = 4,
	VE_PIX_NV21 = 5,
	VE_PIX_NV12 = 6,
	VE_PIX_YUV_MB32_420 = 7,
	VE_PIX_YUV_MB32_422 = 8,
	VE_PIX_YUV_MB32_444 = 9,
	VE_PIX_RGBA = 10,
	VE_PIX_ARGB = 11,
	VE_PIX_ABGR = 12,
	VE_PIX_BGRA = 13,
};

/* Picture buffer type a frame buffer manager is created for */
enum ve_buffer_type {
	VE_BUF_REFERENCE_DISPLAY = 0,
	VE_BUF_ONLY_REFERENCE = 1,
	VE_BUF_ONLY_DISPLAY = 2,
};

/* Results of ve_decode() / DecodeVideoStream() */
enum ve_decode_result {
	VE_RESULT_UNSUPPORTED = -1,
	VE_RESULT_OK = 0,
	VE_RESULT_FRAME_DECODED = 1,
	VE_RESULT_CONTINUE = 2,
	VE_RESULT_KEYFRAME_DECODED = 3,
	VE_RESULT_NO_FRAME_BUFFER = 4,
	VE_RESULT_NO_BITSTREAM = 5,
	VE_RESULT_RESOLUTION_CHANGE = 6,
};

/* ---- memory operations the decoders allocate through ---- */

struct ve_mem_ops {
	int (*open)(void);
	void (*close)(void);
	int (*total_size)(void);
	void *(*palloc)(int size, void *veops, void *self);
	void *(*palloc_no_cache)(int size, void *veops, void *self);
	void (*pfree)(void *mem, void *veops, void *self);
	void (*flush_cache)(void *mem, int size);
	void *(*ve_get_phyaddr)(void *virt);
	void *(*ve_get_viraddr)(void *phys);
	void *(*cpu_get_phyaddr)(void *virt);
	void *(*cpu_get_viraddr)(void *phys);
	int (*mem_set)(void *s, int c, size_t n);
	int (*mem_cpy)(void *dst, void *src, size_t n);
	int (*mem_read)(void *dst, void *src, size_t n);
	int (*mem_write)(void *dst, void *src, size_t n);
	int (*setup)(void);
	int (*shutdown)(void);
	unsigned int (*get_ve_addr_offset)(void);
};

/* ---- engine access table (video engine registers, lock, interrupt) ---- */

struct ve_config {
	int decoder_flag;
	int encoder_flag;
	int format;
	int width;
	int enable_afbc;
	int reset_mode;
	unsigned int ve_freq;
	struct ve_mem_ops *memops;
};

struct ve_user_iommu_param {
	int fd;
	unsigned int iommu_addr;
};

struct ve_ops {
	void *(*init)(struct ve_config *cfg);
	void (*release)(void *self);
	int (*lock)(void *self);
	int (*unlock)(void *self);
	void (*reset)(void *self);
	int (*wait_interrupt)(void *self);
	int (*get_chip_id)(void *self);
	uint64_t (*get_ic_ve_version)(void *self);
	void *(*get_group_reg_addr)(void *self, int group);
	int (*get_dram_type)(void *self);
	unsigned int (*get_phy_offset)(void *self);
	void (*set_dram_type)(void *self);
	void (*set_ddr_mode)(void *self, int mode);
	int (*set_speed)(void *self, unsigned int mhz);
	void (*set_enable_afbc_flag)(void *self, int flag);
	void (*set_adjust_dram_speed_flag)(void *self, int flag);
	void (*enable_ve)(void *self);
	void (*disable_ve)(void *self);
	void (*init_encoder_performance)(void *self, int flag);
	void (*uninit_encoder_performance)(void *self, int flag);
	int (*get_iommu_addr)(void *self, struct ve_user_iommu_param *param);
	int (*free_iommu_addr)(void *self, struct ve_user_iommu_param *param);
	int (*set_proc_info)(void *self, char *info, unsigned int len, unsigned char channel);
	int (*stop_proc_info)(void *self, unsigned char channel);
};

/* ---- stream and decoder configuration ---- */

struct ve_stream_info {
	int codec_format;
	int width;
	int height;
	int frame_rate;
	int frame_duration;
	int aspect_ratio;
	int is_3d_stream;
	int csd_len;
	char *csd;
	int secure_stream;
	int secure_stream_level1;
	int is_frame_package;
	int h265_ref_pic_num;
	int reopen_engine;
	int is_frame_cts_test;
	int is_raw_stream;
};

struct ve_vconfig {
	int scale_down_en;
	int rotation_en;
	int sec_output_en;
	int horizon_scale_down_ratio;
	int vertical_scale_down_ratio;
	int sec_horizon_scale_down_ratio;
	int sec_vertical_scale_down_ratio;
	int rotate_degree;
	int thumbnail_mode;
	int output_pixel_format;
	int sec_output_pixel_format;
	int no_b_frames;
	int disable_3d;
	int support_maf;
	int disp_error_frame;
	int vbv_buffer_size;
	int frame_buffer_num;
	int secure_os_en;
	int gpu_buf_valid;
	int align_stride;
	int is_soft_decoder;
	int vir_malloc_sbm;
	int support_palloc_buf_before_decode;
	int deinterlace_holding_fb_num;
	int display_holding_fb_num;
	int rotate_holding_fb_num;
	int decode_smooth_fb_num;
	int is_tv_stream;
	int lbc_lossy_com_mod;
	unsigned int is_lossy;
	unsigned int rc_en;
	int max_memory_available;
	struct ve_mem_ops *memops;
	int ctl_afbc_mode;
	int ctl_iptv_mode;
	struct ve_ops *ve_ops;
	void *ve_ops_self;
	int convert_vp9_10bit_to_8bit;
	unsigned int ve_freq;
	int called_by_omx;
	int set_proc_info_enable;
	int set_proc_info_freq;
	int channel_num;
	int support_max_width;
	int support_max_height;
	int common_config_flag;
};

struct ve_stream_data {
	char *data;
	int length;
	int64_t pts;
	int64_t pcr;
	int is_first_part;
	int is_last_part;
	int id;
	int stream_index;
	int valid;
	unsigned int video_info_flag;
	void *video_info;
};

/* ---- pictures and the frame buffer manager ---- */

struct ve_mv_info {
	int16_t max_mv_x;
	int16_t min_mv_x;
	int16_t avg_mv_x;
	int16_t max_mv_y;
	int16_t min_mv_y;
	int16_t avg_mv_y;
	int16_t max_mv;
	int16_t min_mv;
	int16_t avg_mv;
	int16_t skip_ratio;
};

struct ve_frame_status {
	int frame_type;
	int size;
	int display_w;
	int display_h;
	int qp;
	double average_bit_rate;
	double frame_rate;
	int64_t pts;
	struct ve_mv_info mv;
	int drop_previous;
};

struct ve_picture {
	int id;
	int stream_index;
	int pixel_format;
	int width;
	int height;
	int line_stride;
	int top_offset;
	int left_offset;
	int bottom_offset;
	int right_offset;
	int frame_rate;
	int aspect_ratio;
	int is_progressive;
	int top_field_first;
	int repeat_top_field;
	int64_t pts;
	int64_t pcr;
	char *data0;
	char *data1;
	char *data2;
	char *data3;
	int maf_valid;
	char *maf_data;
	int maf_flag_stride;
	int pre_frame_valid;
	int buf_id;
	uintptr_t phy_y_addr;
	uintptr_t phy_c_addr;
	void *priv;
	int buf_fd;
	int buf_status;
	int top_field_error;
	int bottom_field_error;
	int color_primary;
	int frame_error;
	void *meta_data;
	int full_range_flag;
	int transfer_characteristics;
	int matrix_coeffs;
	uint8_t colour_primaries;
	int lower2bit_buf_size;
	int lower2bit_buf_offset;
	int lower2bit_buf_stride;
	int is_10bit;
	int afbc_enabled;
	int lbc_lossy_com_mod;
	unsigned int is_lossy;
	unsigned int rc_en;
	int buf_size;
	int afbc_size;
	int lbc_size;
	int debug_count;
	struct ve_frame_status cur_frame;
	int output_mb32;
};

struct ve_fbm_buf_info {
	int buf_num;
	int buf_width;
	int buf_height;
	int pixel_format;
	int align_value;
	int progressive;
	int is_soft_decoder;
	int hdr_video;
	int is_10bit_video;
	int afbc_mode;
	int lbc_lossy_com_mod;
	unsigned int is_lossy;
	unsigned int rc_en;
	int top_offset;
	int bottom_offset;
	int left_offset;
	int right_offset;
};

struct ve_fbm_info {
	unsigned int valid_buf_num;
	void *fbm_first;
	void *fbm_second;
	struct ve_fbm_buf_info buf_info;
	unsigned int is_3d_stream;
	unsigned int two_stream_share_one_fbm;
	struct ve_picture *major_disp_frame;
	struct ve_picture *major_decoder_frame;
	unsigned int minor_y_buf_offset;
	unsigned int minor_c_buf_offset;
	int is_frame_cts_test;
	int extra_fbm_buffer_num;
	int decoder_needed_mini_fbm_num;
	int decoder_needed_mini_fbm_num_sd;
	int is_soft_decoder;
};

struct ve_fbm_node_flag {
	int used_by_decoder;
	int used_by_render;
	int in_valid_picture_queue;
	int already_displayed;
	int need_release;
};

struct ve_fbm_node {
	struct ve_fbm_node_flag flag;
	struct ve_picture picture;
	struct ve_fbm_node *next;
};

struct ve_fbm_create_info {
	int frame_num;
	int decoder_needed_mini_frame_num;
	int width;
	int height;
	int pixel_format;
	int thumbnail_mode;
	int gpu_buf_valid;
	int align_stride;
	int buffer_type;
	int progressive;
	int is_soft_decoder;
	struct ve_mem_ops *memops;
	int is_10bit_stream;
	struct ve_ops *ve_ops;
	void *ve_ops_self;
};

/*
 * The archive's mutex type is a 40 byte block that is all zero when idle. The
 * glue layer keeps a k_mutex pointer in its first word.
 */
struct ve_fbm {
	uint32_t mutex[10];
	int max_frame_num;
	int empty_buffer_num;
	int valid_picture_num;
	int release_buffer_num;
	struct ve_fbm_node *empty_queue;
	struct ve_fbm_node *valid_queue;
	struct ve_fbm_node *release_queue;
	int thumbnail_mode;
	struct ve_fbm_node *frames;
	int use_gpu_buf;
	int align_value;
	void *fbm_info;
	int decoder_holding_num;
	int render_holding_num;
	int wait_for_disp_num;
	struct ve_mem_ops *memops;
	struct ve_ops *ve_ops;
	void *ve_ops_self;
	int show_log;
};

/* ---- stream buffer manager ---- */

struct ve_sbm_config {
	int vir_flag;
	int sbm_buffer_total_size;
	struct ve_mem_ops *memops;
	struct ve_ops *ve_ops;
	void *ve_ops_self;
	int secure_video;
	int width;
	int config_sbm_buffer_size;
	int nalu_length;
};

enum ve_sbm_type {
	VE_SBM_STREAM = 0,
	VE_SBM_FRAME = 1,
};

struct ve_sbm {
	int (*init)(struct ve_sbm *self, struct ve_sbm_config *cfg);
	void (*destroy)(struct ve_sbm *self);
	void (*reset)(struct ve_sbm *self);
	void *(*get_buffer_address)(struct ve_sbm *self);
	int (*get_buffer_size)(struct ve_sbm *self);
	int (*get_stream_frame_num)(struct ve_sbm *self);
	int (*get_stream_data_size)(struct ve_sbm *self);
	int (*request_buffer)(struct ve_sbm *self, int size, char **buf, int *buf_size);
	int (*add_stream)(struct ve_sbm *self, struct ve_stream_data *data);
	struct ve_stream_data *(*request_stream)(struct ve_sbm *self);
	int (*return_stream)(struct ve_sbm *self, struct ve_stream_data *data);
	int (*flush_stream)(struct ve_sbm *self, struct ve_stream_data *data);
	char *(*get_buffer_write_pointer)(struct ve_sbm *self);
	void *(*get_buffer_data_info)(struct ve_sbm *self);
	int (*set_eos)(struct ve_sbm *self, int eos);
	int type;
	int use_new_ve_memory_program;
};

/* ---- decoder instance created by the archive ---- */

struct ve_decoder_perf_info {
	unsigned int drop_frame_num;
	int frame_duration;
};

struct ve_decoder_if {
	int (*init)(struct ve_decoder_if *self, struct ve_vconfig *cfg, struct ve_stream_info *info,
		    struct ve_fbm_info *fbm_info);
	void (*reset)(struct ve_decoder_if *self);
	int (*set_sbm)(struct ve_decoder_if *self, struct ve_sbm *sbm, int index);
	int (*get_fbm_num)(struct ve_decoder_if *self);
	struct ve_fbm *(*get_fbm)(struct ve_decoder_if *self, int index);
	int (*decode)(struct ve_decoder_if *self, int end_of_stream, int key_frame_only,
		      int skip_b_frame_if_delay, int64_t current_time_us);
	void (*destroy)(struct ve_decoder_if *self);
	int (*set_special_data)(struct ve_decoder_if *self, void *arg);
	int (*set_extra_scale_info)(struct ve_decoder_if *self, int width_th, int height_th,
				    int horizon_ratio, int vertical_ratio);
	int (*set_rotate_info)(struct ve_decoder_if *self, int32_t degree);
	int (*set_perform_cmd)(struct ve_decoder_if *self, int cmd);
	int (*get_perform_info)(struct ve_decoder_if *self, int cmd,
				struct ve_decoder_perf_info **info);
};

struct ve_engine {
	struct ve_vconfig vconfig;
	struct ve_stream_info stream_info;
	void *lib_handle;
	struct ve_decoder_if *decoder;
	int is_software_decoder;
	struct ve_fbm_info fbm_info;
	uint8_t reset_ve_mode;
	struct ve_ops *ve_ops;
	void *ve_ops_self;
	uint64_t ic_ve_version;
	uint32_t dec_ip_version;
	struct ve_sbm *sbm[2];
	int32_t enable_google_vp9;
	int tr_handle;
	int tr_channel;
	int init_tr;
};

/* ---- provided by this driver for the archive and the decoder front end ---- */

struct ve_fifo_node;

void FIFOEnqueue(struct ve_fifo_node **head, struct ve_fifo_node *node);
struct ve_fifo_node *FIFODequeue(struct ve_fifo_node **head);
void FIFOEnqueueToHead(struct ve_fifo_node **head, struct ve_fifo_node *node);

struct ve_fbm *FbmCreate(struct ve_fbm_create_info *ci, struct ve_fbm_info *info);
void FbmDestroy(struct ve_fbm *fbm);
struct ve_picture *FbmRequestBuffer(struct ve_fbm *fbm);
void FbmReturnBuffer(struct ve_fbm *fbm, struct ve_picture *pic, int valid);
void FbmShareBuffer(struct ve_fbm *fbm, struct ve_picture *pic);
struct ve_picture *FbmRequestPicture(struct ve_fbm *fbm);
int FbmReturnPicture(struct ve_fbm *fbm, struct ve_picture *pic);
struct ve_picture *FbmNextPictureInfo(struct ve_fbm *fbm);
void FbmFlush(struct ve_fbm *fbm);
int FbmTotalBufferNum(struct ve_fbm *fbm);
int FbmEmptyBufferNum(struct ve_fbm *fbm);
int FbmValidPictureNum(struct ve_fbm *fbm);
int FbmGetDisplayBufferNum(struct ve_fbm *fbm);
int FbmGetAlignValue(struct ve_fbm *fbm);
unsigned int FbmGetBufferOffset(struct ve_fbm *fbm, int is_y_buf);
int FbmGetBufferInfo(struct ve_fbm *fbm, struct ve_picture *pic);

/* ---- entry points of the archive ---- */

struct ve_ops *GetVeOpsS(int type);
struct ve_engine *VideoEngineCreate(struct ve_vconfig *cfg, struct ve_stream_info *info);
void VideoEngineDestroy(struct ve_engine *engine);
void VideoEngineReset(struct ve_engine *engine);
int VideoEngineSetSbm(struct ve_engine *engine, struct ve_sbm *sbm, int index);
int VideoEngineGetFbmNum(struct ve_engine *engine);
struct ve_fbm *VideoEngineGetFbm(struct ve_engine *engine, int index);
int VideoEngineDecode(struct ve_engine *engine, int end_of_stream, int key_frame_only,
		      int drop_b_frame_if_delay, int64_t current_time_us);
int VideoEngineReopen(struct ve_engine *engine, struct ve_vconfig *cfg,
		      struct ve_stream_info *info);
int VideoEngineConvert(struct ve_ops *ops, void *ops_self, struct ve_picture *in,
		       struct ve_picture *out);
int GetBufferSize(int pixel_format, int width, int height, int *y_size, int *c_size,
		  int *y_stride, int *c_stride, int align);
void CedarPluginVDInit_mjpeg(void);
void CedarPluginVDInit_png(void);
void CedarPluginVDInit_h264(void);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_DRIVERS_VDEC_SUNXI_VE_ABI_H_ */

/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later */
/*
 * Display pipeline framework - kernel mode setting objects.
 *
 * The object model mirrors DRM KMS:
 *
 *   plane(s) --> crtc --> encoder --> [bridge ...] --> connector/panel
 *
 * - a plane scans out a framebuffer region into a CRTC;
 * - a CRTC composes its planes and produces the timing of one display
 *   output (provided here by a display engine mixer + a timing controller);
 * - an encoder converts the pixel stream to an interface (RGB/LVDS/DSI);
 * - a connector represents the sink and knows its modes (from a panel).
 *
 * All configuration goes through atomic state objects: a commit duplicates
 * the current state of the touched objects, modifies the copies, checks
 * them with the drivers and only then applies them to the hardware.
 */
#ifndef __DPY_KMS_H__
#define __DPY_KMS_H__

#include <hal/display/display_engine.h>
#include <dpy/dpy_device.h>

struct dpy_atomic_state;
struct dpy_crtc;
struct dpy_encoder;
struct dpy_connector;
struct dpy_panel;
struct dpy_bridge;
struct dpy_timing_ctrl;

#define DPY_MAX_PLANES		16
#define DPY_MAX_CRTCS		2
#define DPY_MAX_ENCODERS	4
#define DPY_MAX_CONNECTORS	4
#define DPY_MAX_MODES		DISPLAY_MAX_MODES

/* ------------------------------------------------------------------ */
/* Formats                                                             */
/* ------------------------------------------------------------------ */
struct dpy_format_info {
	uint32_t format;	/* enum display_format */
	const char *name;
	uint8_t num_planes;
	uint8_t cpp[3];		/* bytes per sample of each plane */
	uint8_t hsub;		/* horizontal chroma subsampling */
	uint8_t vsub;		/* vertical chroma subsampling */
	bool is_yuv;
	bool has_alpha;
};

const struct dpy_format_info *dpy_format_info(uint32_t format);

/* ------------------------------------------------------------------ */
/* Modes and bus formats                                               */
/* ------------------------------------------------------------------ */
struct dpy_display_mode {
	uint32_t clock;		/* pixel clock, kHz */
	uint16_t hdisplay, hsync_start, hsync_end, htotal;
	uint16_t vdisplay, vsync_start, vsync_end, vtotal;
	uint32_t flags;		/* DISPLAY_MODE_FLAG_* */
};

uint32_t dpy_mode_vrefresh_mhz(const struct dpy_display_mode *m);
bool dpy_mode_equal(const struct dpy_display_mode *a,
		    const struct dpy_display_mode *b);
void dpy_mode_to_api(const struct dpy_display_mode *m,
		     struct display_mode *out);
void dpy_mode_from_api(const struct display_mode *in,
		       struct dpy_display_mode *m);
bool dpy_mode_valid(const struct dpy_display_mode *m);

/* pixel formats on the wire between encoder and panel */
enum dpy_bus_format {
	DPY_BUS_FMT_NONE = 0,
	DPY_BUS_FMT_RGB888_1X24,
	DPY_BUS_FMT_RGB666_1X18,
	DPY_BUS_FMT_RGB565_1X16,
	DPY_BUS_FMT_RGB666_1X7X3_SPWG,	/* LVDS 6 bit */
	DPY_BUS_FMT_RGB888_1X7X4_SPWG,	/* LVDS 8 bit VESA */
	DPY_BUS_FMT_RGB888_1X7X4_JEIDA,	/* LVDS 8 bit JEIDA */
};

/* bus flags describing signal polarities */
#define DPY_BUS_FLAG_DE_LOW		(1U << 0)
#define DPY_BUS_FLAG_PIXDATA_NEGEDGE	(1U << 1)	/* data driven on falling edge */
#define DPY_BUS_FLAG_DATA_INVERT	(1U << 2)

/* bits per colour component carried by a bus format */
uint32_t dpy_bus_format_bpc(uint32_t bus_format);

struct dpy_display_info {
	uint32_t width_mm;
	uint32_t height_mm;
	uint32_t bus_format;	/* enum dpy_bus_format */
	uint32_t bus_flags;	/* DPY_BUS_FLAG_* */
};

/* ------------------------------------------------------------------ */
/* Framebuffer description (memory is owned by the caller)             */
/* ------------------------------------------------------------------ */
struct dpy_fb {
	uint32_t format;
	const struct dpy_format_info *info;
	uint32_t width;
	uint32_t height;
	dpy_dma_addr_t addr[3];
	uint32_t pitch[3];
};

/* ------------------------------------------------------------------ */
/* Planes                                                              */
/* ------------------------------------------------------------------ */
enum dpy_plane_type {
	DPY_PLANE_PRIMARY,
	DPY_PLANE_OVERLAY,
};

struct dpy_plane_state {
	struct dpy_plane *plane;
	struct dpy_crtc *crtc;		/* NULL: plane disabled */
	struct dpy_fb fb;
	struct dpy_rect_fp src;		/* 16.16 */
	struct dpy_rect dst;
	uint8_t alpha;
	uint8_t zpos;
	uint8_t blend_mode;		/* enum display_blend_mode */
	uint8_t color_encoding;
	uint8_t color_range;

	/* computed by dpy_plane_helper_check_state() */
	bool visible;
	struct dpy_rect_fp src_clip;
	struct dpy_rect dst_clip;
	uint32_t normalized_zpos;
};

struct dpy_plane_funcs {
	/* optional; allow drivers to subclass the state */
	struct dpy_plane_state *(*duplicate_state)(struct dpy_plane *plane);
	void (*destroy_state)(struct dpy_plane *plane,
			      struct dpy_plane_state *state);
	int (*atomic_check)(struct dpy_plane *plane,
			    struct dpy_atomic_state *state);
	void (*atomic_update)(struct dpy_plane *plane,
			      struct dpy_atomic_state *state);
	void (*atomic_disable)(struct dpy_plane *plane,
			       struct dpy_atomic_state *state);
};

struct dpy_plane {
	struct dpy_device *dev;
	struct dpy_list head;
	uint32_t index;
	const char *name;
	enum dpy_plane_type type;
	uint32_t possible_crtcs;
	const uint32_t *formats;
	uint32_t num_formats;
	uint32_t caps;			/* DISPLAY_PLANE_CAP_* */
	uint32_t group;			/* hardware channel */
	uint32_t group_index;		/* layer inside the channel */
	uint32_t max_width;
	uint32_t max_height;
	struct dpy_plane_state *state;
	const struct dpy_plane_funcs *funcs;
	void *priv;
};

bool dpy_plane_has_format(const struct dpy_plane *plane, uint32_t format);

/* ------------------------------------------------------------------ */
/* CRTCs                                                               */
/* ------------------------------------------------------------------ */
struct dpy_color_adjust {
	uint8_t brightness;	/* 0..100, 50 neutral */
	uint8_t contrast;
	uint8_t saturation;
	uint8_t hue;
};

#define DPY_GAMMA_SIZE	256

struct dpy_gamma_lut {
	uint16_t r[DPY_GAMMA_SIZE];
	uint16_t g[DPY_GAMMA_SIZE];
	uint16_t b[DPY_GAMMA_SIZE];
};

struct dpy_crtc_state {
	struct dpy_crtc *crtc;
	bool enable;
	bool active;
	struct dpy_display_mode mode;

	/* change tracking, computed during the check phase */
	bool mode_changed;
	bool active_changed;
	bool connectors_changed;
	bool planes_changed;
	bool color_changed;

	uint32_t plane_mask;		/* BIT(plane->index) */
	uint32_t connector_mask;
	uint32_t encoder_mask;

	/* sink description, filled by the encoder during check */
	uint32_t bus_format;
	uint32_t bus_flags;

	uint32_t background;		/* 0xAARRGGBB */
	struct dpy_color_adjust adjust;
	bool gamma_enable;
	uint32_t gamma_seq;		/* bumped when the LUT content changes */

	/* hardware state has to be latched, wait for the flip */
	bool flip_needed;
};

struct dpy_crtc_funcs {
	struct dpy_crtc_state *(*duplicate_state)(struct dpy_crtc *crtc);
	void (*destroy_state)(struct dpy_crtc *crtc,
			      struct dpy_crtc_state *state);
	int (*atomic_check)(struct dpy_crtc *crtc,
			    struct dpy_atomic_state *state);
	void (*atomic_enable)(struct dpy_crtc *crtc,
			      struct dpy_atomic_state *state);
	void (*atomic_disable)(struct dpy_crtc *crtc,
			       struct dpy_atomic_state *state);
	void (*atomic_begin)(struct dpy_crtc *crtc,
			     struct dpy_atomic_state *state);
	/* program the hardware; latching is signalled by dpy_crtc_flip_done() */
	void (*atomic_flush)(struct dpy_crtc *crtc,
			     struct dpy_atomic_state *state);
	/* IRQ context, called on every vblank before waiters are woken */
	void (*vblank)(struct dpy_crtc *crtc);
	/*
	 * IRQ context, a few lines into every frame: registers latched at the
	 * frame start are visible now, so a flip may complete here instead of
	 * waiting for the next vblank. Optional.
	 */
	void (*frame_start)(struct dpy_crtc *crtc);
	/* debug */
	void (*dump)(struct dpy_crtc *crtc,
		     void (*print)(const char *fmt, ...));
};

struct dpy_crtc {
	struct dpy_device *dev;
	struct dpy_list head;
	uint32_t index;
	const char *name;
	struct dpy_crtc_state *state;
	const struct dpy_crtc_funcs *funcs;
	void *priv;

	/* position in the graph: engine node + output port */
	const struct dpy_gnode *node;
	uint8_t port;
	/* timing controller bound to this crtc (vblank source) */
	struct dpy_timing_ctrl *tc;

	struct dpy_gamma_lut *gamma;	/* LUT storage, see gamma_seq */

	/* vblank bookkeeping, protected by lock */
	dpy_spinlock_t lock;
	uint64_t vblank_count;
	uint32_t vblank_waiters;
	struct dpy_sem *vblank_sem;
	uint32_t vblank_refcount;
	uint64_t vblank_last_us;
	uint32_t refresh_mhz;

	/* flip completion */
	bool flip_pending;
	uint32_t flip_vblanks;
	struct dpy_sem *flip_sem;

	/* statistics */
	uint32_t commit_count;
	uint32_t commit_timeout;
	uint32_t last_commit_us;
};

/* ------------------------------------------------------------------ */
/* Encoders and connectors                                             */
/* ------------------------------------------------------------------ */
enum dpy_encoder_type {
	DPY_ENCODER_DPI,
	DPY_ENCODER_LVDS,
	DPY_ENCODER_DSI,
	DPY_ENCODER_VIRTUAL,
};

struct dpy_connector_state {
	struct dpy_connector *connector;
	struct dpy_crtc *crtc;
	struct dpy_encoder *encoder;
	struct dpy_writeback_job *wb_job;
};

struct dpy_encoder_funcs {
	int (*atomic_check)(struct dpy_encoder *encoder,
			    struct dpy_crtc_state *crtc_state,
			    struct dpy_connector_state *conn_state);
	void (*atomic_mode_set)(struct dpy_encoder *encoder,
				struct dpy_crtc_state *crtc_state,
				struct dpy_connector_state *conn_state);
	void (*atomic_enable)(struct dpy_encoder *encoder,
			      struct dpy_atomic_state *state);
	void (*atomic_disable)(struct dpy_encoder *encoder,
			       struct dpy_atomic_state *state);
};

struct dpy_encoder {
	struct dpy_device *dev;
	struct dpy_list head;
	uint32_t index;
	const char *name;
	enum dpy_encoder_type type;
	uint32_t possible_crtcs;
	const struct dpy_encoder_funcs *funcs;
	void *priv;
	const struct dpy_gnode *node;
	struct dpy_bridge *bridge;	/* first bridge of the chain */
	struct dpy_crtc *crtc;		/* crtc driving it while enabled */
};

enum dpy_connector_type {
	DPY_CONNECTOR_DPI,
	DPY_CONNECTOR_LVDS,
	DPY_CONNECTOR_DSI,
	DPY_CONNECTOR_WRITEBACK,
};

struct dpy_connector_funcs {
	int (*get_modes)(struct dpy_connector *connector);
	int (*atomic_check)(struct dpy_connector *connector,
			    struct dpy_atomic_state *state);
};

struct dpy_connector {
	struct dpy_device *dev;
	struct dpy_list head;
	uint32_t index;
	const char *name;
	enum dpy_connector_type type;
	struct dpy_connector_state *state;
	const struct dpy_connector_funcs *funcs;
	void *priv;

	struct dpy_encoder *encoder;
	struct dpy_panel *panel;
	struct dpy_display_info info;
	struct dpy_display_mode modes[DPY_MAX_MODES];
	uint32_t num_modes;
	uint32_t preferred;
};

/* one-shot capture request carried by a writeback connector state */
struct dpy_writeback_job {
	struct dpy_fb fb;
	struct dpy_rect src;
	struct dpy_rect dst;
	int status;
	struct dpy_sem *done;
};

/* ------------------------------------------------------------------ */
/* The display device                                                  */
/* ------------------------------------------------------------------ */
struct dpy_device {
	struct dpy_mutex *lock;		/* serialises commits (modeset lock) */
	struct dpy_list planes;
	struct dpy_list crtcs;
	struct dpy_list encoders;
	struct dpy_list connectors;
	uint32_t num_planes;
	uint32_t num_crtcs;
	uint32_t num_encoders;
	uint32_t num_connectors;
	uint32_t max_width;
	uint32_t max_height;

	bool registered;
	uint32_t open_count;
};

/* ---- object registration (called from component bind) ---- */
int dpy_plane_init(struct dpy_device *dev, struct dpy_plane *plane,
		   const struct dpy_plane_funcs *funcs,
		   enum dpy_plane_type type, const uint32_t *formats,
		   uint32_t num_formats, uint32_t possible_crtcs,
		   const char *name);
int dpy_crtc_init(struct dpy_device *dev, struct dpy_crtc *crtc,
		  const struct dpy_crtc_funcs *funcs, const char *name);
int dpy_encoder_init(struct dpy_device *dev, struct dpy_encoder *encoder,
		     const struct dpy_encoder_funcs *funcs,
		     enum dpy_encoder_type type, const char *name);
int dpy_connector_init(struct dpy_device *dev,
		       struct dpy_connector *connector,
		       const struct dpy_connector_funcs *funcs,
		       enum dpy_connector_type type, const char *name);
void dpy_plane_cleanup(struct dpy_plane *plane);
void dpy_crtc_cleanup(struct dpy_crtc *crtc);
void dpy_encoder_cleanup(struct dpy_encoder *encoder);
void dpy_connector_cleanup(struct dpy_connector *connector);

struct dpy_crtc *dpy_crtc_from_index(struct dpy_device *dev, uint32_t idx);
struct dpy_plane *dpy_plane_from_index(struct dpy_device *dev, uint32_t idx);
/* crtc created for output @port of engine node @node */
struct dpy_crtc *dpy_crtc_find_by_port(struct dpy_device *dev,
				       const struct dpy_gnode *node,
				       uint8_t port);
/* compute encoder->possible_crtcs from the graph (encoder <- tcon <- de) */
uint32_t dpy_encoder_graph_possible_crtcs(struct dpy_device *dev,
					  const struct dpy_gnode *encoder_node);

/* refresh connector->modes from its panel / get_modes() */
int dpy_connector_update_modes(struct dpy_connector *connector);

/* ---- vblank and flip completion ---- */
/* called by the vblank source (IRQ context) */
void dpy_crtc_handle_vblank(struct dpy_crtc *crtc);
/* called by the vblank source at the early line of a frame (IRQ context) */
void dpy_crtc_handle_frame_start(struct dpy_crtc *crtc);
int dpy_crtc_vblank_get(struct dpy_crtc *crtc);
void dpy_crtc_vblank_put(struct dpy_crtc *crtc);
int dpy_crtc_wait_vblank(struct dpy_crtc *crtc, uint32_t timeout_ms);
uint64_t dpy_crtc_vblank_count(struct dpy_crtc *crtc);
/* driver side of the flip protocol */
void dpy_crtc_arm_flip(struct dpy_crtc *crtc);
/*
 * Split form for drivers that must arm the flip in the same critical
 * section (crtc->lock held) that starts the hardware update.
 */
void dpy_crtc_flip_drain(struct dpy_crtc *crtc);
void dpy_crtc_arm_flip_locked(struct dpy_crtc *crtc);
void dpy_crtc_flip_done(struct dpy_crtc *crtc);	/* IRQ safe */
int dpy_crtc_wait_flip(struct dpy_crtc *crtc, uint32_t timeout_ms);

/* ---- plane helpers ---- */
/*
 * Clip the plane against the crtc mode, compute visibility, check the
 * scaling factor against [min_scale, max_scale] (16.16, src/dst).
 */
int dpy_plane_helper_check_state(struct dpy_plane_state *ps,
				 const struct dpy_crtc_state *cs,
				 int32_t min_scale, int32_t max_scale);

/* ------------------------------------------------------------------ */
/* Atomic state                                                        */
/* ------------------------------------------------------------------ */
struct dpy_atomic_plane {
	struct dpy_plane *ptr;
	struct dpy_plane_state *old_state, *new_state;
};

struct dpy_atomic_crtc {
	struct dpy_crtc *ptr;
	struct dpy_crtc_state *old_state, *new_state;
};

struct dpy_atomic_connector {
	struct dpy_connector *ptr;
	struct dpy_connector_state *old_state, *new_state;
};

#define DPY_COMMIT_TEST_ONLY	(1U << 0)
#define DPY_COMMIT_NONBLOCK	(1U << 1)
#define DPY_COMMIT_ALLOW_MODESET (1U << 2)

struct dpy_atomic_state {
	struct dpy_device *dev;
	uint32_t flags;
	bool checked;
	bool swapped;
	struct dpy_atomic_plane planes[DPY_MAX_PLANES];
	struct dpy_atomic_crtc crtcs[DPY_MAX_CRTCS];
	struct dpy_atomic_connector connectors[DPY_MAX_CONNECTORS];
};

struct dpy_atomic_state *dpy_atomic_state_alloc(struct dpy_device *dev);
void dpy_atomic_state_free(struct dpy_atomic_state *state);

/* get (duplicating the current state if needed) a new state to modify */
struct dpy_plane_state *dpy_atomic_get_plane_state(struct dpy_atomic_state *s,
						   struct dpy_plane *plane);
struct dpy_crtc_state *dpy_atomic_get_crtc_state(struct dpy_atomic_state *s,
						 struct dpy_crtc *crtc);
struct dpy_connector_state *
dpy_atomic_get_connector_state(struct dpy_atomic_state *s,
			       struct dpy_connector *connector);

/* lookups without duplication, NULL if the object is not in the state */
struct dpy_plane_state *dpy_atomic_new_plane_state(struct dpy_atomic_state *s,
						   struct dpy_plane *plane);
struct dpy_plane_state *dpy_atomic_old_plane_state(struct dpy_atomic_state *s,
						   struct dpy_plane *plane);
struct dpy_crtc_state *dpy_atomic_new_crtc_state(struct dpy_atomic_state *s,
						 struct dpy_crtc *crtc);
struct dpy_crtc_state *dpy_atomic_old_crtc_state(struct dpy_atomic_state *s,
						 struct dpy_crtc *crtc);

/* pull every plane currently or newly assigned to @crtc into the state */
int dpy_atomic_add_affected_planes(struct dpy_atomic_state *s,
				   struct dpy_crtc *crtc);

int dpy_atomic_check(struct dpy_atomic_state *s);
/* check (if not done) and apply; consumes @s in every case */
int dpy_atomic_commit(struct dpy_atomic_state *s, uint32_t flags);
/* switch every crtc and plane off */
int dpy_atomic_disable_all(struct dpy_device *dev);

#define dpy_for_each_plane_in_state(s, i, pl)				\
	for ((i) = 0; (i) < DPY_MAX_PLANES; (i)++)			\
		if (((pl) = &(s)->planes[i])->ptr)

#define dpy_for_each_crtc_in_state(s, i, c)				\
	for ((i) = 0; (i) < DPY_MAX_CRTCS; (i)++)			\
		if (((c) = &(s)->crtcs[i])->ptr)

#define dpy_for_each_connector_in_state(s, i, c)			\
	for ((i) = 0; (i) < DPY_MAX_CONNECTORS; (i)++)			\
		if (((c) = &(s)->connectors[i])->ptr)

/* default state helpers usable by drivers that subclass the state */
void dpy_plane_state_copy(struct dpy_plane_state *dst,
			  const struct dpy_plane_state *src);
void dpy_crtc_state_copy(struct dpy_crtc_state *dst,
			 const struct dpy_crtc_state *src);

#endif /* __DPY_KMS_H__ */

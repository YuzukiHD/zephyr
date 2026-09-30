// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Native API of the display stack (hal/display/display_engine.h).
 *
 * Every call is translated into an atomic state update of display 0:
 * the current state is duplicated, modified according to the request,
 * checked by the drivers and committed on the next vertical blank.
 */
#define DPY_LOG_TAG "api"
#include <dpy/dpy_device.h>
#include <dpy/dpy_kms.h>
#include <dpy/dpy_log.h>
#include <dpy/dpy_panel.h>
#include <dpy/dpy_tcon.h>

extern const struct dpy_soc_desc dpy_soc;
extern const struct dpy_board_desc dpy_board;

#define DPY_API_VBLANK_TIMEOUT_MS	100

static struct dpy_device *dpy_api_dev(void)
{
	return dpy_device_get();
}

static struct dpy_crtc *dpy_api_crtc(struct dpy_device *dev)
{
	return dev ? dpy_crtc_from_index(dev, 0) : NULL;
}

/* first real (non writeback) connector driven by or drivable from @crtc */
static struct dpy_connector *dpy_api_connector(struct dpy_device *dev)
{
	struct dpy_connector *conn;

	if (!dev)
		return NULL;
	dpy_list_for_each_entry(conn, &dev->connectors, head)
		if (conn->type != DPY_CONNECTOR_WRITEBACK && conn->encoder)
			return conn;
	return NULL;
}

static struct dpy_connector *dpy_api_writeback(struct dpy_device *dev)
{
	struct dpy_connector *conn;

	if (!dev)
		return NULL;
	dpy_list_for_each_entry(conn, &dev->connectors, head)
		if (conn->type == DPY_CONNECTOR_WRITEBACK)
			return conn;
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Life cycle                                                          */
/* ------------------------------------------------------------------ */
int display_probe(void)
{
	return dpy_core_probe(&dpy_soc, &dpy_board);
}

int display_remove(void)
{
	dpy_core_remove();
	return 0;
}

int display_wait_ready(uint32_t timeout_ms)
{
	return dpy_core_wait_ready(timeout_ms);
}

int display_open(void)
{
	struct dpy_device *dev = dpy_api_dev();

	if (!dev)
		return -ENODEV;
	dpy_os_mutex_lock(dev->lock);
	dev->open_count++;
	dpy_os_mutex_unlock(dev->lock);
	return 0;
}

int display_close(void)
{
	struct dpy_device *dev = dpy_api_dev();

	if (!dev)
		return -ENODEV;
	dpy_os_mutex_lock(dev->lock);
	if (dev->open_count)
		dev->open_count--;
	dpy_os_mutex_unlock(dev->lock);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Queries                                                             */
/* ------------------------------------------------------------------ */
static const char *dpy_encoder_type_name(enum dpy_encoder_type t)
{
	switch (t) {
	case DPY_ENCODER_DPI:
		return "rgb";
	case DPY_ENCODER_LVDS:
		return "lvds";
	case DPY_ENCODER_DSI:
		return "dsi";
	default:
		return "virtual";
	}
}

int display_get_caps(struct display_caps *caps)
{
	struct dpy_device *dev = dpy_api_dev();
	struct dpy_connector *conn = dpy_api_connector(dev);

	if (!dev || !caps)
		return -ENODEV;
	memset(caps, 0, sizeof(*caps));
	caps->plane_count = dev->num_planes;
	caps->writeback = dpy_api_writeback(dev) != NULL;
	if (conn) {
		caps->mode_count = conn->num_modes;
		caps->width_mm = conn->info.width_mm;
		caps->height_mm = conn->info.height_mm;
		caps->interface = dpy_encoder_type_name(conn->encoder->type);
		caps->panel = conn->panel ? conn->panel->name : NULL;
	}
	return 0;
}

int display_get_plane_caps(uint32_t plane_id, struct display_plane_caps *caps)
{
	struct dpy_device *dev = dpy_api_dev();
	struct dpy_plane *plane;
	uint32_t i;

	if (!dev || !caps)
		return -ENODEV;
	plane = dpy_plane_from_index(dev, plane_id);
	if (!plane)
		return -EINVAL;
	memset(caps, 0, sizeof(*caps));
	caps->plane_id = plane->index;
	caps->channel = plane->group;
	caps->layer = plane->group_index;
	caps->flags = plane->caps;
	caps->max_width = plane->max_width;
	caps->max_height = plane->max_height;
	for (i = 0; i < plane->num_formats && i < DISPLAY_FORMAT_COUNT; i++)
		caps->formats[i] = (enum display_format)plane->formats[i];
	caps->format_count = i;
	return 0;
}

int display_get_mode(struct display_mode *mode)
{
	struct dpy_crtc *crtc = dpy_api_crtc(dpy_api_dev());

	if (!crtc || !mode)
		return -ENODEV;
	if (!crtc->state->enable)
		return -ENODEV;
	dpy_mode_to_api(&crtc->state->mode, mode);
	return 0;
}

int display_get_modes(struct display_mode *modes, uint32_t max)
{
	struct dpy_connector *conn = dpy_api_connector(dpy_api_dev());
	uint32_t i;

	if (!conn || !modes)
		return -ENODEV;
	for (i = 0; i < conn->num_modes && i < max; i++)
		dpy_mode_to_api(&conn->modes[i], &modes[i]);
	return (int)i;
}

static void dpy_fb_to_api(const struct dpy_fb *fb,
			  struct display_framebuffer *out)
{
	uint32_t i;

	memset(out, 0, sizeof(*out));
	out->format = (enum display_format)fb->format;
	out->width = fb->width;
	out->height = fb->height;
	out->plane_count = fb->info ? fb->info->num_planes : 0;
	for (i = 0; i < out->plane_count; i++) {
		out->plane_address[i] = fb->addr[i];
		out->plane_stride[i] = fb->pitch[i];
	}
	out->address = fb->addr[0];
	out->stride = fb->pitch[0];
}

int display_get_state(struct display_pipeline_state *state)
{
	struct dpy_device *dev = dpy_api_dev();
	struct dpy_crtc *crtc = dpy_api_crtc(dev);
	struct dpy_plane *plane;
	uint32_t n = 0;

	if (!crtc || !state)
		return -ENODEV;
	memset(state, 0, sizeof(*state));

	dpy_os_mutex_lock(dev->lock);
	dpy_mode_to_api(&crtc->state->mode, &state->mode);
	state->active = crtc->state->active;
	state->background_argb = crtc->state->background;
	dpy_list_for_each_entry(plane, &dev->planes, head) {
		const struct dpy_plane_state *ps = plane->state;
		struct display_plane_state *out;

		if (n >= DISPLAY_MAX_PLANES)
			break;
		out = &state->planes[n++];
		out->plane_id = plane->index;
		out->enable = ps->crtc == crtc;
		out->alpha = ps->alpha;
		out->zpos = ps->zpos;
		out->blend_mode = ps->blend_mode;
		out->color_encoding = ps->color_encoding;
		out->color_range = ps->color_range;
		dpy_fb_to_api(&ps->fb, &out->framebuffer);
		out->source.x = ps->src.x >> 16;
		out->source.y = ps->src.y >> 16;
		out->source.width = ps->src.w >> 16;
		out->source.height = ps->src.h >> 16;
		out->destination.x = ps->dst.x;
		out->destination.y = ps->dst.y;
		out->destination.width = ps->dst.w;
		out->destination.height = ps->dst.h;
	}
	state->plane_count = n;
	dpy_os_mutex_unlock(dev->lock);
	return 0;
}

void display_pipeline_state_init(struct display_pipeline_state *state)
{
	uint32_t i;

	memset(state, 0, sizeof(*state));
	state->active = true;
	state->background_argb = 0xff000000;
	for (i = 0; i < DISPLAY_MAX_PLANES; i++) {
		state->planes[i].plane_id = i;
		state->planes[i].alpha = 0xff;
		state->planes[i].blend_mode = DISPLAY_BLEND_COVERAGE;
	}
}

/* ------------------------------------------------------------------ */
/* Submit                                                              */
/* ------------------------------------------------------------------ */
static int dpy_api_fill_fb(const struct display_framebuffer *in,
			   struct dpy_fb *fb)
{
	const struct dpy_format_info *info = dpy_format_info(in->format);
	uintptr_t cpu_addr[3] = { 0 };
	uint32_t i, h;

	if (!info || !in->width || !in->height)
		return -EINVAL;

	memset(fb, 0, sizeof(*fb));
	fb->format = in->format;
	fb->info = info;
	fb->width = in->width;
	fb->height = in->height;

	for (i = 0; i < info->num_planes; i++) {
		uint32_t pitch = in->plane_stride[i];

		if (!pitch && i == 0)
			pitch = in->stride;
		if (!pitch && i > 0 && fb->pitch[0])
			pitch = fb->pitch[0] * info->cpp[i] / info->hsub /
				info->cpp[0];
		if (!pitch)
			pitch = DPY_DIV_ROUND_UP(in->width, i ? info->hsub : 1) *
				info->cpp[i];
		fb->pitch[i] = pitch;

		cpu_addr[i] = in->plane_address[i];
		if (!cpu_addr[i] && i == 0)
			cpu_addr[i] = in->address;
		if (!cpu_addr[i] && i > 0 && cpu_addr[i - 1]) {
			/* planes stored back to back */
			h = i == 1 ? in->height
				   : DPY_DIV_ROUND_UP(in->height, info->vsub);
			cpu_addr[i] = cpu_addr[i - 1] + (uintptr_t)fb->pitch[i - 1] * h;
		}
		if (!cpu_addr[i])
			return -EINVAL;
		fb->addr[i] = dpy_os_virt_to_dma((const void *)cpu_addr[i]);
	}
	return 0;
}

static int dpy_api_fill_plane(const struct display_plane_state *in,
			      struct dpy_plane_state *ps, struct dpy_crtc *crtc,
			      const struct dpy_display_mode *mode)
{
	uint32_t sw, sh;
	int ret;

	if (!in->enable) {
		ps->crtc = NULL;
		return 0;
	}

	ret = dpy_api_fill_fb(&in->framebuffer, &ps->fb);
	if (ret)
		return ret;

	sw = in->source.width ? in->source.width : ps->fb.width;
	sh = in->source.height ? in->source.height : ps->fb.height;
	ps->src.x = DPY_FP16(in->source.x);
	ps->src.y = DPY_FP16(in->source.y);
	ps->src.w = (uint32_t)DPY_FP16(sw);
	ps->src.h = (uint32_t)DPY_FP16(sh);

	ps->dst.x = in->destination.x;
	ps->dst.y = in->destination.y;
	ps->dst.w = in->destination.width ? in->destination.width
					  : mode->hdisplay;
	ps->dst.h = in->destination.height ? in->destination.height
					   : mode->vdisplay;

	ps->crtc = crtc;
	ps->alpha = in->alpha;
	ps->zpos = in->zpos;
	ps->blend_mode = in->blend_mode;
	ps->color_encoding = in->color_encoding;
	ps->color_range = in->color_range;
	return 0;
}

/* resolve a requested mode against the connector mode list */
static int dpy_api_pick_mode(struct dpy_connector *conn,
			     const struct display_mode *req,
			     struct dpy_display_mode *out)
{
	struct dpy_display_mode m;
	uint32_t i;

	dpy_mode_from_api(req, &m);
	for (i = 0; i < conn->num_modes; i++) {
		const struct dpy_display_mode *c = &conn->modes[i];

		if (c->hdisplay != m.hdisplay || c->vdisplay != m.vdisplay)
			continue;
		/* a partially filled request matches on size (and refresh) */
		if (!req->htotal || !req->vtotal || !req->clock_khz) {
			if (req->refresh_hz &&
			    (dpy_mode_vrefresh_mhz(c) + 500) / 1000 !=
			    req->refresh_hz)
				continue;
			*out = *c;
			return 0;
		}
		if (dpy_mode_equal(c, &m)) {
			*out = *c;
			return 0;
		}
	}
	return -EINVAL;
}

int display_submit_ex(const struct display_pipeline_state *state,
		      uint32_t flags)
{
	struct dpy_device *dev = dpy_api_dev();
	struct dpy_crtc *crtc = dpy_api_crtc(dev);
	struct dpy_connector *conn = dpy_api_connector(dev);
	struct dpy_connector_state *conn_state;
	struct dpy_crtc_state *cs;
	struct dpy_atomic_state *s;
	struct dpy_plane *plane;
	uint32_t commit_flags = DPY_COMMIT_ALLOW_MODESET;
	uint32_t listed = 0, i;
	int ret;

	if (!crtc || !conn || !state)
		return -ENODEV;
	if (state->plane_count > DISPLAY_MAX_PLANES)
		return -EINVAL;

	s = dpy_atomic_state_alloc(dev);
	if (!s)
		return -ENOMEM;

	cs = dpy_atomic_get_crtc_state(s, crtc);
	conn_state = dpy_atomic_get_connector_state(s, conn);
	if (!cs || !conn_state) {
		ret = -ENOMEM;
		goto err;
	}

	if (state->mode.width && state->mode.height) {
		struct dpy_display_mode mode;

		ret = dpy_api_pick_mode(conn, &state->mode, &mode);
		if (ret) {
			dpy_err("mode %ux%u not supported by %s\n",
				state->mode.width, state->mode.height,
				conn->name);
			goto err;
		}
		cs->mode = mode;
	}
	if (!(flags & DISPLAY_SUBMIT_PARTIAL)) {
		cs->enable = true;
		cs->active = state->active;
		cs->background = state->background_argb | 0xff000000;
		conn_state->crtc = crtc;
		conn_state->encoder = conn->encoder;
	}

	for (i = 0; i < state->plane_count; i++) {
		const struct display_plane_state *in = &state->planes[i];
		struct dpy_plane_state *ps;

		plane = dpy_plane_from_index(dev, in->plane_id);
		if (!plane) {
			ret = -EINVAL;
			goto err;
		}
		ps = dpy_atomic_get_plane_state(s, plane);
		if (!ps) {
			ret = -ENOMEM;
			goto err;
		}
		ret = dpy_api_fill_plane(in, ps, crtc, &cs->mode);
		if (ret) {
			dpy_err("plane %u: invalid framebuffer\n", in->plane_id);
			goto err;
		}
		listed |= DPY_BIT(plane->index);
	}

	/* a full update turns off every plane it does not mention */
	if (!(flags & DISPLAY_SUBMIT_PARTIAL)) {
		dpy_list_for_each_entry(plane, &dev->planes, head) {
			struct dpy_plane_state *ps;

			if ((listed & DPY_BIT(plane->index)) ||
			    plane->state->crtc != crtc)
				continue;
			ps = dpy_atomic_get_plane_state(s, plane);
			if (!ps) {
				ret = -ENOMEM;
				goto err;
			}
			ps->crtc = NULL;
		}
	}

	if (flags & DISPLAY_SUBMIT_TEST_ONLY)
		commit_flags |= DPY_COMMIT_TEST_ONLY;
	if (flags & DISPLAY_SUBMIT_NONBLOCK)
		commit_flags |= DPY_COMMIT_NONBLOCK;
	return dpy_atomic_commit(s, commit_flags);

err:
	dpy_atomic_state_free(s);
	return ret;
}

int display_submit(const struct display_pipeline_state *state)
{
	return display_submit_ex(state, 0);
}

int display_wait_vsync(uint32_t timeout_ms)
{
	struct dpy_crtc *crtc = dpy_api_crtc(dpy_api_dev());

	if (!crtc)
		return -ENODEV;
	return dpy_crtc_wait_vblank(crtc, timeout_ms);
}

/* commit a crtc-only property change */
static int dpy_api_crtc_update(void (*fn)(struct dpy_crtc_state *cs,
					  const void *arg),
			       const void *arg)
{
	struct dpy_device *dev = dpy_api_dev();
	struct dpy_crtc *crtc = dpy_api_crtc(dev);
	struct dpy_crtc_state *cs;
	struct dpy_atomic_state *s;

	if (!crtc)
		return -ENODEV;
	s = dpy_atomic_state_alloc(dev);
	if (!s)
		return -ENOMEM;
	cs = dpy_atomic_get_crtc_state(s, crtc);
	if (!cs) {
		dpy_atomic_state_free(s);
		return -ENOMEM;
	}
	fn(cs, arg);
	return dpy_atomic_commit(s, DPY_COMMIT_ALLOW_MODESET);
}

static void dpy_api_set_active(struct dpy_crtc_state *cs, const void *arg)
{
	cs->active = !*(const bool *)arg;
}

int display_blank(bool blank)
{
	return dpy_api_crtc_update(dpy_api_set_active, &blank);
}

static void dpy_api_set_adjust(struct dpy_crtc_state *cs, const void *arg)
{
	const struct display_color_adjust *adj = arg;

	cs->adjust.brightness = DPY_MIN(adj->brightness, 100);
	cs->adjust.contrast = DPY_MIN(adj->contrast, 100);
	cs->adjust.saturation = DPY_MIN(adj->saturation, 100);
	cs->adjust.hue = DPY_MIN(adj->hue, 100);
}

int display_set_color_adjust(const struct display_color_adjust *adj)
{
	if (!adj)
		return -EINVAL;
	return dpy_api_crtc_update(dpy_api_set_adjust, adj);
}

int display_get_color_adjust(struct display_color_adjust *adj)
{
	struct dpy_crtc *crtc = dpy_api_crtc(dpy_api_dev());

	if (!crtc || !adj)
		return -ENODEV;
	adj->brightness = crtc->state->adjust.brightness;
	adj->contrast = crtc->state->adjust.contrast;
	adj->saturation = crtc->state->adjust.saturation;
	adj->hue = crtc->state->adjust.hue;
	return 0;
}

static void dpy_api_set_bg(struct dpy_crtc_state *cs, const void *arg)
{
	cs->background = *(const uint32_t *)arg | 0xff000000;
}

int display_set_background(uint32_t argb)
{
	return dpy_api_crtc_update(dpy_api_set_bg, &argb);
}

struct dpy_api_gamma {
	const uint16_t *lut;
	uint32_t entries;
};

/* runs with the device lock held, so the shared table is not torn */
static void dpy_api_set_gamma(struct dpy_crtc_state *cs, const void *arg)
{
	const struct dpy_api_gamma *g = arg;
	struct dpy_gamma_lut *dst = cs->crtc->gamma;
	uint32_t i, src;

	if (g->lut) {
		/* resample the caller's table to the LUT size */
		for (i = 0; i < DPY_GAMMA_SIZE; i++) {
			src = i * (g->entries - 1) / (DPY_GAMMA_SIZE - 1);
			dst->r[i] = g->lut[src];
			dst->g[i] = g->lut[g->entries + src];
			dst->b[i] = g->lut[2 * g->entries + src];
		}
	}
	cs->gamma_enable = g->lut != NULL;
	cs->gamma_seq++;
}

int display_set_gamma(const uint16_t *lut, uint32_t entries)
{
	struct dpy_api_gamma g = { lut, entries };

	if (lut && entries < 2)
		return -EINVAL;
	return dpy_api_crtc_update(dpy_api_set_gamma, &g);
}

/* ------------------------------------------------------------------ */
/* Backlight, test pattern                                             */
/* ------------------------------------------------------------------ */
static struct dpy_backlight *dpy_api_backlight(void)
{
	struct dpy_connector *conn = dpy_api_connector(dpy_api_dev());

	return conn && conn->panel ? conn->panel->backlight : NULL;
}

int display_set_backlight(uint32_t level)
{
	struct dpy_device *dev = dpy_api_dev();
	struct dpy_backlight *bl = dpy_api_backlight();
	int ret;

	if (!bl)
		return -ENODEV;
	/* serialise against commits switching the panel on or off */
	dpy_os_mutex_lock(dev->lock);
	ret = dpy_backlight_set_level(bl, DPY_MIN(level, DISPLAY_BACKLIGHT_MAX) *
					  bl->max_level / DISPLAY_BACKLIGHT_MAX);
	dpy_os_mutex_unlock(dev->lock);
	return ret;
}

int display_get_backlight(uint32_t *level)
{
	struct dpy_backlight *bl = dpy_api_backlight();

	if (!bl || !level)
		return -ENODEV;
	*level = bl->level * DISPLAY_BACKLIGHT_MAX / bl->max_level;
	return 0;
}

int display_set_test_pattern(uint32_t pattern)
{
	struct dpy_crtc *crtc = dpy_api_crtc(dpy_api_dev());

	if (!crtc || !crtc->tc || !crtc->tc->ops->set_pattern)
		return -ENODEV;
	return crtc->tc->ops->set_pattern(crtc->tc, pattern);
}

/* ------------------------------------------------------------------ */
/* Capture                                                             */
/* ------------------------------------------------------------------ */
int display_capture(const struct display_capture_req *req)
{
	struct dpy_device *dev = dpy_api_dev();
	struct dpy_crtc *crtc = dpy_api_crtc(dev);
	struct dpy_connector *wb = dpy_api_writeback(dev);
	struct dpy_connector_state *st;
	struct dpy_writeback_job job;
	struct dpy_atomic_state *s;
	uint32_t timeout;
	int ret;

	if (!crtc || !wb || !req)
		return -ENODEV;
	if (!crtc->state->active)
		return -EINVAL;

	memset(&job, 0, sizeof(job));
	ret = dpy_api_fill_fb(&req->framebuffer, &job.fb);
	if (ret)
		return ret;
	job.src.x = req->source.x;
	job.src.y = req->source.y;
	job.src.w = req->source.width ? req->source.width
				      : crtc->state->mode.hdisplay;
	job.src.h = req->source.height ? req->source.height
				       : crtc->state->mode.vdisplay;
	job.dst.x = req->destination.x;
	job.dst.y = req->destination.y;
	job.dst.w = req->destination.width ? req->destination.width
					   : job.fb.width;
	job.dst.h = req->destination.height ? req->destination.height
					    : job.fb.height;
	job.status = -EINPROGRESS;
	job.done = dpy_os_sem_create(0);
	if (!job.done)
		return -ENOMEM;

	s = dpy_atomic_state_alloc(dev);
	if (!s) {
		ret = -ENOMEM;
		goto out;
	}
	st = dpy_atomic_get_connector_state(s, wb);
	if (!st || !dpy_atomic_get_crtc_state(s, crtc)) {
		dpy_atomic_state_free(s);
		ret = -ENOMEM;
		goto out;
	}
	st->crtc = crtc;
	st->wb_job = &job;
	ret = dpy_atomic_commit(s, 0);
	if (ret)
		goto out;

	timeout = req->timeout_ms ? req->timeout_ms : 200;
	ret = dpy_os_sem_wait(job.done, timeout);
	if (!ret)
		ret = job.status;
out:
	dpy_os_sem_destroy(job.done);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Statistics                                                          */
/* ------------------------------------------------------------------ */
int display_get_stats(struct display_stats *stats)
{
	struct dpy_crtc *crtc = dpy_api_crtc(dpy_api_dev());

	if (!crtc || !stats)
		return -ENODEV;
	memset(stats, 0, sizeof(*stats));
	stats->vblank_count = dpy_crtc_vblank_count(crtc);
	stats->commit_count = crtc->commit_count;
	stats->commit_timeout = crtc->commit_timeout;
	stats->last_commit_us = crtc->last_commit_us;
	stats->refresh_mhz = crtc->refresh_mhz;
	if (crtc->tc)
		stats->fifo_underflow = crtc->tc->underflow_count;
	return 0;
}

// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Display pipeline framework - KMS object registration, vblank handling
 * and plane helpers.
 */
#define DPY_LOG_TAG "kms"
#include <dpy/dpy_kms.h>
#include <dpy/dpy_log.h>
#include <dpy/dpy_panel.h>
#include <dpy/dpy_tcon.h>

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */
int dpy_plane_init(struct dpy_device *dev, struct dpy_plane *plane,
		   const struct dpy_plane_funcs *funcs,
		   enum dpy_plane_type type, const uint32_t *formats,
		   uint32_t num_formats, uint32_t possible_crtcs,
		   const char *name)
{
	struct dpy_plane_state *st;

	if (dev->num_planes >= DPY_MAX_PLANES)
		return -ENOSPC;

	st = funcs->duplicate_state ? funcs->duplicate_state(plane)
				    : dpy_os_zalloc(sizeof(*st));
	if (!st)
		return -ENOMEM;
	st->plane = plane;
	st->alpha = 0xff;

	plane->dev = dev;
	plane->index = dev->num_planes++;
	plane->name = name;
	plane->type = type;
	plane->formats = formats;
	plane->num_formats = num_formats;
	plane->possible_crtcs = possible_crtcs;
	plane->funcs = funcs;
	plane->state = st;
	dpy_list_add_tail(&plane->head, &dev->planes);
	return 0;
}

int dpy_crtc_init(struct dpy_device *dev, struct dpy_crtc *crtc,
		  const struct dpy_crtc_funcs *funcs, const char *name)
{
	struct dpy_crtc_state *st;

	if (dev->num_crtcs >= DPY_MAX_CRTCS)
		return -ENOSPC;

	st = funcs->duplicate_state ? funcs->duplicate_state(crtc)
				    : dpy_os_zalloc(sizeof(*st));
	if (!st)
		return -ENOMEM;
	st->crtc = crtc;
	st->background = 0xff000000;
	st->adjust.brightness = 50;
	st->adjust.contrast = 50;
	st->adjust.saturation = 50;
	st->adjust.hue = 50;

	crtc->vblank_sem = dpy_os_sem_create(0);
	crtc->flip_sem = dpy_os_sem_create(0);
	crtc->gamma = dpy_os_zalloc(sizeof(*crtc->gamma));
	if (!crtc->vblank_sem || !crtc->flip_sem || !crtc->gamma) {
		dpy_os_sem_destroy(crtc->vblank_sem);
		dpy_os_sem_destroy(crtc->flip_sem);
		dpy_os_free(crtc->gamma);
		if (funcs->destroy_state)
			funcs->destroy_state(crtc, st);
		else
			dpy_os_free(st);
		return -ENOMEM;
	}
	dpy_os_spin_init(&crtc->lock);

	crtc->dev = dev;
	crtc->index = dev->num_crtcs++;
	crtc->name = name;
	crtc->funcs = funcs;
	crtc->state = st;
	dpy_list_add_tail(&crtc->head, &dev->crtcs);
	return 0;
}

int dpy_encoder_init(struct dpy_device *dev, struct dpy_encoder *encoder,
		     const struct dpy_encoder_funcs *funcs,
		     enum dpy_encoder_type type, const char *name)
{
	if (dev->num_encoders >= DPY_MAX_ENCODERS)
		return -ENOSPC;
	encoder->dev = dev;
	encoder->index = dev->num_encoders++;
	encoder->name = name;
	encoder->type = type;
	encoder->funcs = funcs;
	dpy_list_add_tail(&encoder->head, &dev->encoders);
	return 0;
}

int dpy_connector_init(struct dpy_device *dev,
		       struct dpy_connector *connector,
		       const struct dpy_connector_funcs *funcs,
		       enum dpy_connector_type type, const char *name)
{
	struct dpy_connector_state *st;

	if (dev->num_connectors >= DPY_MAX_CONNECTORS)
		return -ENOSPC;
	st = dpy_os_zalloc(sizeof(*st));
	if (!st)
		return -ENOMEM;
	st->connector = connector;
	connector->dev = dev;
	connector->index = dev->num_connectors++;
	connector->name = name;
	connector->type = type;
	connector->funcs = funcs;
	connector->state = st;
	dpy_list_add_tail(&connector->head, &dev->connectors);
	return 0;
}

void dpy_plane_cleanup(struct dpy_plane *plane)
{
	dpy_list_del(&plane->head);
	if (plane->funcs->destroy_state)
		plane->funcs->destroy_state(plane, plane->state);
	else
		dpy_os_free(plane->state);
	plane->state = NULL;
	plane->dev->num_planes--;
}

void dpy_crtc_cleanup(struct dpy_crtc *crtc)
{
	dpy_list_del(&crtc->head);
	if (crtc->funcs->destroy_state)
		crtc->funcs->destroy_state(crtc, crtc->state);
	else
		dpy_os_free(crtc->state);
	crtc->state = NULL;
	dpy_os_sem_destroy(crtc->vblank_sem);
	dpy_os_sem_destroy(crtc->flip_sem);
	dpy_os_free(crtc->gamma);
	crtc->dev->num_crtcs--;
}

void dpy_encoder_cleanup(struct dpy_encoder *encoder)
{
	dpy_list_del(&encoder->head);
	encoder->dev->num_encoders--;
}

void dpy_connector_cleanup(struct dpy_connector *connector)
{
	dpy_list_del(&connector->head);
	dpy_os_free(connector->state);
	connector->state = NULL;
	connector->dev->num_connectors--;
}

bool dpy_plane_has_format(const struct dpy_plane *plane, uint32_t format)
{
	uint32_t i;

	for (i = 0; i < plane->num_formats; i++)
		if (plane->formats[i] == format)
			return true;
	return false;
}

struct dpy_crtc *dpy_crtc_from_index(struct dpy_device *dev, uint32_t idx)
{
	struct dpy_crtc *crtc;

	dpy_list_for_each_entry(crtc, &dev->crtcs, head)
		if (crtc->index == idx)
			return crtc;
	return NULL;
}

struct dpy_plane *dpy_plane_from_index(struct dpy_device *dev, uint32_t idx)
{
	struct dpy_plane *plane;

	dpy_list_for_each_entry(plane, &dev->planes, head)
		if (plane->index == idx)
			return plane;
	return NULL;
}

struct dpy_crtc *dpy_crtc_find_by_port(struct dpy_device *dev,
				       const struct dpy_gnode *node,
				       uint8_t port)
{
	struct dpy_crtc *crtc;

	dpy_list_for_each_entry(crtc, &dev->crtcs, head)
		if (crtc->node == node && crtc->port == port)
			return crtc;
	return NULL;
}

/*
 * encoder input port 0 -> timing controller -> timing controller input
 * port 0 -> display engine output port N: the CRTC of that port.
 */
uint32_t dpy_encoder_graph_possible_crtcs(struct dpy_device *dev,
					  const struct dpy_gnode *encoder_node)
{
	const struct dpy_gnode *tcon, *engine;
	struct dpy_crtc *crtc;
	unsigned int iter = 0;
	uint8_t engine_port;
	uint32_t mask = 0;

	while ((tcon = dpy_node_remote_next(encoder_node, 0, &iter, NULL,
					    NULL))) {
		engine = dpy_node_remote(tcon, 0, DPY_EP_ANY, &engine_port,
					 NULL);
		if (!engine)
			continue;
		crtc = dpy_crtc_find_by_port(dev, engine, engine_port);
		if (crtc)
			mask |= DPY_BIT(crtc->index);
	}
	return mask;
}

int dpy_connector_update_modes(struct dpy_connector *connector)
{
	int n = 0;
	uint32_t i;

	if (connector->funcs && connector->funcs->get_modes)
		n = connector->funcs->get_modes(connector);
	else if (connector->panel)
		n = dpy_panel_get_modes(connector->panel, connector->modes,
					DPY_MAX_MODES);
	if (n < 0)
		n = 0;
	connector->num_modes = (uint32_t)n;
	connector->preferred = 0;
	for (i = 0; i < connector->num_modes; i++) {
		if (connector->modes[i].flags & DISPLAY_MODE_FLAG_PREFERRED) {
			connector->preferred = i;
			break;
		}
	}
	return n;
}

/* ------------------------------------------------------------------ */
/* Vblank                                                              */
/* ------------------------------------------------------------------ */
void dpy_crtc_handle_frame_start(struct dpy_crtc *crtc)
{
	if (crtc->funcs->frame_start)
		crtc->funcs->frame_start(crtc);
}

void dpy_crtc_handle_vblank(struct dpy_crtc *crtc)
{
	unsigned long flags;
	uint32_t waiters, i;
	uint64_t now;

	if (crtc->funcs->vblank)
		crtc->funcs->vblank(crtc);

	now = dpy_os_time_us();
	flags = dpy_os_spin_lock_irqsave(&crtc->lock);
	crtc->vblank_count++;
	if (crtc->vblank_last_us && now > crtc->vblank_last_us) {
		uint32_t mhz = (uint32_t)(1000000000ULL /
					  (now - crtc->vblank_last_us));
		/* simple low pass filter */
		crtc->refresh_mhz = crtc->refresh_mhz ?
				    (crtc->refresh_mhz * 7 + mhz) / 8 : mhz;
	}
	crtc->vblank_last_us = now;
	if (crtc->flip_pending)
		crtc->flip_vblanks++;
	waiters = crtc->vblank_waiters;
	crtc->vblank_waiters = 0;
	dpy_os_spin_unlock_irqrestore(&crtc->lock, flags);

	for (i = 0; i < waiters; i++)
		dpy_os_sem_post(crtc->vblank_sem);
}

int dpy_crtc_vblank_get(struct dpy_crtc *crtc)
{
	unsigned long flags;
	bool first;

	flags = dpy_os_spin_lock_irqsave(&crtc->lock);
	first = crtc->vblank_refcount++ == 0;
	dpy_os_spin_unlock_irqrestore(&crtc->lock, flags);
	if (first && crtc->tc)
		return dpy_timing_ctrl_enable_vblank(crtc->tc, true);
	return crtc->tc ? 0 : -ENODEV;
}

void dpy_crtc_vblank_put(struct dpy_crtc *crtc)
{
	unsigned long flags;
	bool last = false;

	flags = dpy_os_spin_lock_irqsave(&crtc->lock);
	if (crtc->vblank_refcount)
		last = --crtc->vblank_refcount == 0;
	dpy_os_spin_unlock_irqrestore(&crtc->lock, flags);
	if (last && crtc->tc)
		dpy_timing_ctrl_enable_vblank(crtc->tc, false);
}

uint64_t dpy_crtc_vblank_count(struct dpy_crtc *crtc)
{
	unsigned long flags;
	uint64_t count;

	flags = dpy_os_spin_lock_irqsave(&crtc->lock);
	count = crtc->vblank_count;
	dpy_os_spin_unlock_irqrestore(&crtc->lock, flags);
	return count;
}

int dpy_crtc_wait_vblank(struct dpy_crtc *crtc, uint32_t timeout_ms)
{
	uint64_t start, deadline, now;
	unsigned long flags;
	uint32_t left;
	int ret;

	if (!crtc->state->active)
		return -EINVAL;

	flags = dpy_os_spin_lock_irqsave(&crtc->lock);
	start = crtc->vblank_count;
	dpy_os_spin_unlock_irqrestore(&crtc->lock, flags);

	deadline = dpy_os_time_us() + (uint64_t)timeout_ms * 1000ULL;
	for (;;) {
		flags = dpy_os_spin_lock_irqsave(&crtc->lock);
		if (crtc->vblank_count != start) {
			dpy_os_spin_unlock_irqrestore(&crtc->lock, flags);
			return 0;
		}
		crtc->vblank_waiters++;
		dpy_os_spin_unlock_irqrestore(&crtc->lock, flags);

		now = dpy_os_time_us();
		if (now >= deadline)
			return -ETIMEDOUT;
		left = (uint32_t)((deadline - now + 999) / 1000);
		ret = dpy_os_sem_wait(crtc->vblank_sem, left);
		if (ret && dpy_crtc_vblank_count(crtc) == start)
			return -ETIMEDOUT;
	}
}

void dpy_crtc_flip_drain(struct dpy_crtc *crtc)
{
	/* drop a completion left over from an earlier timed out flip */
	while (!dpy_os_sem_wait(crtc->flip_sem, 0))
		;
}

void dpy_crtc_arm_flip_locked(struct dpy_crtc *crtc)
{
	crtc->flip_pending = true;
	crtc->flip_vblanks = 0;
}

void dpy_crtc_arm_flip(struct dpy_crtc *crtc)
{
	unsigned long flags;

	dpy_crtc_flip_drain(crtc);
	flags = dpy_os_spin_lock_irqsave(&crtc->lock);
	dpy_crtc_arm_flip_locked(crtc);
	dpy_os_spin_unlock_irqrestore(&crtc->lock, flags);
}

void dpy_crtc_flip_done(struct dpy_crtc *crtc)
{
	unsigned long flags;
	bool was_pending;

	flags = dpy_os_spin_lock_irqsave(&crtc->lock);
	was_pending = crtc->flip_pending;
	crtc->flip_pending = false;
	dpy_os_spin_unlock_irqrestore(&crtc->lock, flags);
	if (was_pending)
		dpy_os_sem_post(crtc->flip_sem);
}

int dpy_crtc_wait_flip(struct dpy_crtc *crtc, uint32_t timeout_ms)
{
	unsigned long flags;
	int ret;

	ret = dpy_os_sem_wait(crtc->flip_sem, timeout_ms);
	if (ret) {
		flags = dpy_os_spin_lock_irqsave(&crtc->lock);
		crtc->flip_pending = false;
		dpy_os_spin_unlock_irqrestore(&crtc->lock, flags);
		crtc->commit_timeout++;
	}
	return ret;
}

/* ------------------------------------------------------------------ */
/* Plane helpers                                                       */
/* ------------------------------------------------------------------ */
static int32_t dpy_scale_factor(uint32_t src_fp, uint32_t dst)
{
	if (!dst)
		return 0;
	return (int32_t)(((uint64_t)src_fp) / dst);
}

int dpy_plane_helper_check_state(struct dpy_plane_state *ps,
				 const struct dpy_crtc_state *cs,
				 int32_t min_scale, int32_t max_scale)
{
	int32_t hscale, vscale;
	int64_t sx, sy, sw, sh;
	int32_t x1, y1, x2, y2;
	int32_t cw, ch;

	ps->visible = false;
	if (!ps->crtc)
		return 0;
	if (!cs || !cs->enable)
		return -EINVAL;
	if (!ps->fb.info || !ps->src.w || !ps->src.h || !ps->dst.w ||
	    !ps->dst.h)
		return -EINVAL;

	/* the source must lie inside the framebuffer */
	if (ps->src.x < 0 || ps->src.y < 0 ||
	    (uint64_t)ps->src.x + ps->src.w > ((uint64_t)ps->fb.width << 16) ||
	    (uint64_t)ps->src.y + ps->src.h > ((uint64_t)ps->fb.height << 16))
		return -ERANGE;

	hscale = dpy_scale_factor(ps->src.w, ps->dst.w);
	vscale = dpy_scale_factor(ps->src.h, ps->dst.h);
	if (hscale < min_scale || hscale > max_scale ||
	    vscale < min_scale || vscale > max_scale)
		return -ERANGE;

	/* clip the destination to the screen and adjust the source */
	cw = cs->mode.hdisplay;
	ch = cs->mode.vdisplay;
	x1 = DPY_MAX(ps->dst.x, 0);
	y1 = DPY_MAX(ps->dst.y, 0);
	x2 = DPY_MIN(ps->dst.x + (int32_t)ps->dst.w, cw);
	y2 = DPY_MIN(ps->dst.y + (int32_t)ps->dst.h, ch);
	if (x2 <= x1 || y2 <= y1)
		return 0;	/* fully off screen: invisible */

	sx = ps->src.x + (int64_t)(x1 - ps->dst.x) * hscale;
	sy = ps->src.y + (int64_t)(y1 - ps->dst.y) * vscale;
	sw = (int64_t)(x2 - x1) * hscale;
	sh = (int64_t)(y2 - y1) * vscale;
	if (x2 - x1 == (int32_t)ps->dst.w)
		sw = ps->src.w;
	if (y2 - y1 == (int32_t)ps->dst.h)
		sh = ps->src.h;

	ps->dst_clip.x = x1;
	ps->dst_clip.y = y1;
	ps->dst_clip.w = (uint32_t)(x2 - x1);
	ps->dst_clip.h = (uint32_t)(y2 - y1);
	ps->src_clip.x = (int32_t)sx;
	ps->src_clip.y = (int32_t)sy;
	ps->src_clip.w = (uint32_t)sw;
	ps->src_clip.h = (uint32_t)sh;
	ps->visible = ps->src_clip.w >= 0x10000 && ps->src_clip.h >= 0x10000;
	return 0;
}

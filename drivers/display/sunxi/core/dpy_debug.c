// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Display pipeline framework - state and register dumps for debugging.
 * The output goes through a printf-like callback so that any shell or
 * log backend can be used.
 */
#include <dpy/dpy_debug.h>
#include <dpy/dpy_kms.h>
#include <dpy/dpy_log.h>
#include <dpy/dpy_panel.h>
#include <dpy/dpy_tcon.h>

static const char *const dpy_encoder_names[] = {
	[DPY_ENCODER_DPI] = "rgb",
	[DPY_ENCODER_LVDS] = "lvds",
	[DPY_ENCODER_DSI] = "dsi",
	[DPY_ENCODER_VIRTUAL] = "virtual",
};

static const char *const dpy_connector_names[] = {
	[DPY_CONNECTOR_DPI] = "dpi",
	[DPY_CONNECTOR_LVDS] = "lvds",
	[DPY_CONNECTOR_DSI] = "dsi",
	[DPY_CONNECTOR_WRITEBACK] = "writeback",
};

static void dpy_debug_mode(dpy_print_t print, const struct dpy_display_mode *m)
{
	uint32_t mhz = dpy_mode_vrefresh_mhz(m);

	print("%ux%u@%u.%03u clk %u kHz h %u/%u/%u/%u v %u/%u/%u/%u%s%s\n",
	      m->hdisplay, m->vdisplay, mhz / 1000, mhz % 1000, m->clock,
	      m->hdisplay, m->hsync_start, m->hsync_end, m->htotal,
	      m->vdisplay, m->vsync_start, m->vsync_end, m->vtotal,
	      m->flags & DISPLAY_MODE_FLAG_NHSYNC ? " -hs" : " +hs",
	      m->flags & DISPLAY_MODE_FLAG_NVSYNC ? " -vs" : " +vs");
}

void dpy_debug_graph(dpy_print_t print)
{
	print("graph:\n");
	dpy_graph_dump(print);
}

void dpy_debug_state(dpy_print_t print)
{
	struct dpy_device *dev = dpy_device_get();
	struct dpy_connector *conn;
	struct dpy_encoder *enc;
	struct dpy_plane *plane;
	struct dpy_crtc *crtc;
	uint32_t i;

	if (!dev) {
		print("display device not bound\n");
		return;
	}

	/* the states must not be swapped and freed while they are printed */
	dpy_os_mutex_lock(dev->lock);
	dpy_list_for_each_entry(crtc, &dev->crtcs, head) {
		const struct dpy_crtc_state *cs = crtc->state;

		print("crtc%u %s: %s, planes %#x, bus fmt %u, bg %08x, bcsh %u/%u/%u/%u, gamma %s\n",
		      crtc->index, crtc->name, cs->active ? "active" : "off",
		      cs->plane_mask, cs->bus_format, cs->background,
		      cs->adjust.brightness, cs->adjust.contrast,
		      cs->adjust.saturation, cs->adjust.hue,
		      cs->gamma_enable ? "on" : "off");
		if (cs->enable) {
			print("  mode ");
			dpy_debug_mode(print, &cs->mode);
		}
		print("  vblank %u, %u.%03u Hz, commits %u (timeouts %u, last %u us), tcon %s\n",
		      (uint32_t)dpy_crtc_vblank_count(crtc),
		      crtc->refresh_mhz / 1000, crtc->refresh_mhz % 1000,
		      crtc->commit_count, crtc->commit_timeout,
		      crtc->last_commit_us, crtc->tc ? crtc->tc->name : "-");
	}

	dpy_list_for_each_entry(plane, &dev->planes, head) {
		const struct dpy_plane_state *ps = plane->state;

		print("plane%-2u %-8s ch%u.%u %s", plane->index, plane->name,
		      plane->group, plane->group_index,
		      plane->type == DPY_PLANE_PRIMARY ? "primary" : "overlay");
		if (!ps->crtc) {
			print(" off\n");
			continue;
		}
		print(" %s %ux%u src %d.%d,%d.%d %ux%u -> %d,%d %ux%u z%u a%u %s\n",
		      display_format_name(ps->fb.format), ps->fb.width,
		      ps->fb.height, ps->src.x >> 16, ps->src.x & 0xffff,
		      ps->src.y >> 16, ps->src.y & 0xffff, ps->src.w >> 16,
		      ps->src.h >> 16, ps->dst.x, ps->dst.y, ps->dst.w,
		      ps->dst.h, ps->normalized_zpos, ps->alpha,
		      ps->visible ? "visible" : "hidden");
	}

	dpy_list_for_each_entry(enc, &dev->encoders, head)
		print("encoder%u %s (%s): crtcs %#x, driving %s\n", enc->index,
		      enc->name, dpy_encoder_names[enc->type],
		      enc->possible_crtcs, enc->crtc ? enc->crtc->name : "-");

	dpy_list_for_each_entry(conn, &dev->connectors, head) {
		print("connector%u %s (%s): %s, %u mode(s), %ux%u mm, bus fmt %u\n",
		      conn->index, conn->name, dpy_connector_names[conn->type],
		      conn->state->crtc ? conn->state->crtc->name : "off",
		      conn->num_modes, conn->info.width_mm,
		      conn->info.height_mm, conn->info.bus_format);
		for (i = 0; i < conn->num_modes; i++) {
			print("  %c ", i == conn->preferred ? '*' : ' ');
			dpy_debug_mode(print, &conn->modes[i]);
		}
		if (conn->panel && conn->panel->backlight)
			print("  backlight %u/%u %s\n",
			      conn->panel->backlight->level,
			      conn->panel->backlight->max_level,
			      conn->panel->backlight->on ? "on" : "off");
	}
	dpy_os_mutex_unlock(dev->lock);
}

void dpy_debug_regs(dpy_print_t print)
{
	struct dpy_device *dev = dpy_device_get();
	struct dpy_crtc *crtc;

	if (!dev) {
		print("display device not bound\n");
		return;
	}
	dpy_list_for_each_entry(crtc, &dev->crtcs, head) {
		if (crtc->funcs->dump)
			crtc->funcs->dump(crtc, print);
		if (crtc->tc && crtc->tc->ops->dump)
			crtc->tc->ops->dump(crtc->tc, print);
	}
}

// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Display pipeline framework - atomic state check and commit.
 */
#define DPY_LOG_TAG "atomic"
#include <dpy/dpy_kms.h>
#include <dpy/dpy_log.h>

/* how long a commit waits for the hardware to latch the new state */
#define DPY_FLIP_TIMEOUT_MS	100

/* ------------------------------------------------------------------ */
/* State allocation                                                    */
/* ------------------------------------------------------------------ */
void dpy_plane_state_copy(struct dpy_plane_state *dst,
			  const struct dpy_plane_state *src)
{
	*dst = *src;
}

void dpy_crtc_state_copy(struct dpy_crtc_state *dst,
			 const struct dpy_crtc_state *src)
{
	*dst = *src;
	dst->mode_changed = false;
	dst->active_changed = false;
	dst->connectors_changed = false;
	dst->planes_changed = false;
	dst->color_changed = false;
	dst->flip_needed = false;
}

static struct dpy_plane_state *dpy_plane_dup(struct dpy_plane *plane)
{
	struct dpy_plane_state *st;

	if (plane->funcs->duplicate_state)
		return plane->funcs->duplicate_state(plane);
	st = dpy_os_zalloc(sizeof(*st));
	if (st)
		dpy_plane_state_copy(st, plane->state);
	return st;
}

static void dpy_plane_destroy(struct dpy_plane *plane,
			      struct dpy_plane_state *st)
{
	if (!st)
		return;
	if (plane->funcs->destroy_state)
		plane->funcs->destroy_state(plane, st);
	else
		dpy_os_free(st);
}

static struct dpy_crtc_state *dpy_crtc_dup(struct dpy_crtc *crtc)
{
	struct dpy_crtc_state *st;

	if (crtc->funcs->duplicate_state)
		return crtc->funcs->duplicate_state(crtc);
	st = dpy_os_zalloc(sizeof(*st));
	if (st)
		dpy_crtc_state_copy(st, crtc->state);
	return st;
}

static void dpy_crtc_destroy(struct dpy_crtc *crtc, struct dpy_crtc_state *st)
{
	if (!st)
		return;
	if (crtc->funcs->destroy_state)
		crtc->funcs->destroy_state(crtc, st);
	else
		dpy_os_free(st);
}

/*
 * A state is built from the current object states, so it owns the device
 * lock from allocation until it is committed or freed: concurrent updates
 * are serialised instead of being built on the same old state. Never
 * allocate a second state while holding one.
 */
struct dpy_atomic_state *dpy_atomic_state_alloc(struct dpy_device *dev)
{
	struct dpy_atomic_state *s = dpy_os_zalloc(sizeof(*s));

	if (!s)
		return NULL;
	s->dev = dev;
	dpy_os_mutex_lock(dev->lock);
	return s;
}

void dpy_atomic_state_free(struct dpy_atomic_state *s)
{
	unsigned int i;

	if (!s)
		return;
	for (i = 0; i < DPY_MAX_PLANES; i++) {
		struct dpy_atomic_plane *p = &s->planes[i];

		if (p->ptr)
			dpy_plane_destroy(p->ptr, s->swapped ? p->old_state
							     : p->new_state);
	}
	for (i = 0; i < DPY_MAX_CRTCS; i++) {
		struct dpy_atomic_crtc *c = &s->crtcs[i];

		if (c->ptr)
			dpy_crtc_destroy(c->ptr, s->swapped ? c->old_state
							    : c->new_state);
	}
	for (i = 0; i < DPY_MAX_CONNECTORS; i++) {
		struct dpy_atomic_connector *c = &s->connectors[i];

		if (c->ptr)
			dpy_os_free(s->swapped ? c->old_state : c->new_state);
	}
	dpy_os_mutex_unlock(s->dev->lock);
	dpy_os_free(s);
}

struct dpy_plane_state *dpy_atomic_get_plane_state(struct dpy_atomic_state *s,
						   struct dpy_plane *plane)
{
	struct dpy_atomic_plane *p = &s->planes[plane->index];

	if (p->ptr)
		return p->new_state;
	p->new_state = dpy_plane_dup(plane);
	if (!p->new_state)
		return NULL;
	p->new_state->plane = plane;
	p->old_state = plane->state;
	p->ptr = plane;
	s->checked = false;
	return p->new_state;
}

struct dpy_crtc_state *dpy_atomic_get_crtc_state(struct dpy_atomic_state *s,
						 struct dpy_crtc *crtc)
{
	struct dpy_atomic_crtc *c = &s->crtcs[crtc->index];

	if (c->ptr)
		return c->new_state;
	c->new_state = dpy_crtc_dup(crtc);
	if (!c->new_state)
		return NULL;
	c->new_state->crtc = crtc;
	c->old_state = crtc->state;
	c->ptr = crtc;
	s->checked = false;
	return c->new_state;
}

struct dpy_connector_state *
dpy_atomic_get_connector_state(struct dpy_atomic_state *s,
			       struct dpy_connector *connector)
{
	struct dpy_atomic_connector *c = &s->connectors[connector->index];

	if (c->ptr)
		return c->new_state;
	c->new_state = dpy_os_zalloc(sizeof(*c->new_state));
	if (!c->new_state)
		return NULL;
	*c->new_state = *connector->state;
	c->new_state->wb_job = NULL;
	c->old_state = connector->state;
	c->ptr = connector;
	s->checked = false;
	return c->new_state;
}

struct dpy_plane_state *dpy_atomic_new_plane_state(struct dpy_atomic_state *s,
						   struct dpy_plane *plane)
{
	return s->planes[plane->index].ptr ? s->planes[plane->index].new_state
					   : NULL;
}

struct dpy_plane_state *dpy_atomic_old_plane_state(struct dpy_atomic_state *s,
						   struct dpy_plane *plane)
{
	return s->planes[plane->index].ptr ? s->planes[plane->index].old_state
					   : NULL;
}

struct dpy_crtc_state *dpy_atomic_new_crtc_state(struct dpy_atomic_state *s,
						 struct dpy_crtc *crtc)
{
	return s->crtcs[crtc->index].ptr ? s->crtcs[crtc->index].new_state
					 : NULL;
}

struct dpy_crtc_state *dpy_atomic_old_crtc_state(struct dpy_atomic_state *s,
						 struct dpy_crtc *crtc)
{
	return s->crtcs[crtc->index].ptr ? s->crtcs[crtc->index].old_state
					 : NULL;
}

int dpy_atomic_add_affected_planes(struct dpy_atomic_state *s,
				   struct dpy_crtc *crtc)
{
	struct dpy_plane_state *ps;
	struct dpy_plane *plane;

	dpy_list_for_each_entry(plane, &s->dev->planes, head) {
		ps = dpy_atomic_new_plane_state(s, plane);
		if (plane->state->crtc != crtc && (!ps || ps->crtc != crtc))
			continue;
		if (!dpy_atomic_get_plane_state(s, plane))
			return -ENOMEM;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Check                                                               */
/* ------------------------------------------------------------------ */
static bool dpy_crtc_needs_modeset(const struct dpy_crtc_state *cs)
{
	return cs->mode_changed || cs->active_changed ||
	       cs->connectors_changed;
}

static int dpy_check_connectors(struct dpy_atomic_state *s)
{
	struct dpy_atomic_connector *ac;
	struct dpy_crtc_state *cs;
	struct dpy_encoder *enc;
	unsigned int i;

	dpy_for_each_connector_in_state(s, i, ac) {
		struct dpy_connector_state *old = ac->old_state;
		struct dpy_connector_state *new = ac->new_state;

		/* a capture tap never changes the output configuration */
		if (ac->ptr->type == DPY_CONNECTOR_WRITEBACK) {
			new->encoder = NULL;
			if (new->wb_job && !new->crtc)
				return -EINVAL;
			if (!new->crtc)
				continue;
			cs = dpy_atomic_get_crtc_state(s, new->crtc);
			if (!cs)
				return -ENOMEM;
			if (new->wb_job && !cs->active)
				return -EINVAL;
			continue;
		}

		if (new->crtc) {
			enc = new->encoder ? new->encoder : ac->ptr->encoder;
			if (!enc || !(enc->possible_crtcs &
				      DPY_BIT(new->crtc->index))) {
				dpy_err("%s cannot be driven by %s\n",
					ac->ptr->name, new->crtc->name);
				return -EINVAL;
			}
			new->encoder = enc;
		} else {
			new->encoder = NULL;
		}

		if (old->crtc != new->crtc) {
			if (old->crtc) {
				cs = dpy_atomic_get_crtc_state(s, old->crtc);
				if (!cs)
					return -ENOMEM;
				cs->connectors_changed = true;
			}
			if (new->crtc) {
				cs = dpy_atomic_get_crtc_state(s, new->crtc);
				if (!cs)
					return -ENOMEM;
				cs->connectors_changed = true;
			}
		}
	}
	return 0;
}

static void dpy_update_crtc_masks(struct dpy_atomic_state *s,
				  struct dpy_crtc_state *cs)
{
	struct dpy_connector *conn;
	struct dpy_plane *plane;

	cs->connector_mask = 0;
	cs->encoder_mask = 0;
	dpy_list_for_each_entry(conn, &s->dev->connectors, head) {
		struct dpy_atomic_connector *ac = &s->connectors[conn->index];
		struct dpy_connector_state *st = ac->ptr ? ac->new_state
							 : conn->state;

		if (st->crtc != cs->crtc || conn->type == DPY_CONNECTOR_WRITEBACK)
			continue;
		cs->connector_mask |= DPY_BIT(conn->index);
		if (st->encoder)
			cs->encoder_mask |= DPY_BIT(st->encoder->index);
	}

	cs->plane_mask = 0;
	dpy_list_for_each_entry(plane, &s->dev->planes, head) {
		struct dpy_plane_state *ps = dpy_atomic_new_plane_state(s, plane);

		if (!ps)
			ps = plane->state;
		if (ps->crtc == cs->crtc)
			cs->plane_mask |= DPY_BIT(plane->index);
	}
}

static int dpy_check_crtcs_modeset(struct dpy_atomic_state *s)
{
	struct dpy_atomic_crtc *ac;
	unsigned int i;

	dpy_for_each_crtc_in_state(s, i, ac) {
		struct dpy_crtc_state *old = ac->old_state;
		struct dpy_crtc_state *new = ac->new_state;

		if (!new->enable)
			new->active = false;
		new->active_changed = old->active != new->active;
		new->mode_changed |= !dpy_mode_equal(&old->mode, &new->mode);
		if (!new->active)
			new->mode_changed = false;

		dpy_update_crtc_masks(s, new);

		if (new->active && !dpy_mode_valid(&new->mode)) {
			dpy_err("%s: invalid mode\n", ac->ptr->name);
			return -EINVAL;
		}
		if (new->active && !new->connector_mask) {
			dpy_err("%s: active without connector\n",
				ac->ptr->name);
			return -EINVAL;
		}
		if (dpy_crtc_needs_modeset(new) &&
		    !(s->flags & DPY_COMMIT_ALLOW_MODESET)) {
			dpy_err("%s: full modeset not allowed\n",
				ac->ptr->name);
			return -EINVAL;
		}
		if (dpy_crtc_needs_modeset(new) ||
		    old->gamma_seq != new->gamma_seq ||
		    old->gamma_enable != new->gamma_enable ||
		    old->background != new->background ||
		    memcmp(&old->adjust, &new->adjust, sizeof(new->adjust)))
			new->color_changed = true;
	}
	return 0;
}

/* a crtc modeset also switches the outputs it drives */
static int dpy_add_affected_connectors(struct dpy_atomic_state *s)
{
	struct dpy_connector *conn;
	struct dpy_atomic_crtc *ac;
	unsigned int i;

	dpy_for_each_crtc_in_state(s, i, ac) {
		if (!dpy_crtc_needs_modeset(ac->new_state))
			continue;
		dpy_list_for_each_entry(conn, &s->dev->connectors, head) {
			if (conn->type == DPY_CONNECTOR_WRITEBACK ||
			    conn->state->crtc != ac->ptr)
				continue;
			if (!dpy_atomic_get_connector_state(s, conn))
				return -ENOMEM;
		}
	}
	return 0;
}

static int dpy_check_encoders(struct dpy_atomic_state *s)
{
	struct dpy_atomic_connector *ac;
	struct dpy_crtc_state *cs;
	struct dpy_encoder *enc;
	unsigned int i;
	int ret;

	dpy_for_each_connector_in_state(s, i, ac) {
		struct dpy_connector_state *new = ac->new_state;

		if (!new->crtc)
			continue;
		cs = dpy_atomic_new_crtc_state(s, new->crtc);
		if (!cs || !cs->active)
			continue;
		enc = new->encoder;
		if (enc && enc->funcs && enc->funcs->atomic_check) {
			ret = enc->funcs->atomic_check(enc, cs, new);
			if (ret) {
				dpy_err("%s: rejected mode: %d\n", enc->name,
					ret);
				return ret;
			}
		}
	}
	return 0;
}

static void dpy_normalize_zpos(struct dpy_atomic_state *s,
			       struct dpy_crtc *crtc)
{
	struct dpy_plane_state *list[DPY_MAX_PLANES];
	struct dpy_plane_state *tmp;
	struct dpy_plane *plane;
	unsigned int n = 0, i, j;

	dpy_list_for_each_entry(plane, &s->dev->planes, head) {
		struct dpy_plane_state *ps = dpy_atomic_new_plane_state(s, plane);

		if (ps && ps->crtc == crtc)
			list[n++] = ps;
	}
	/* insertion sort by (zpos, index): stable and tiny */
	for (i = 1; i < n; i++) {
		tmp = list[i];
		for (j = i; j > 0; j--) {
			struct dpy_plane_state *p = list[j - 1];

			if (p->zpos < tmp->zpos ||
			    (p->zpos == tmp->zpos &&
			     p->plane->index < tmp->plane->index))
				break;
			list[j] = p;
		}
		list[j] = tmp;
	}
	for (i = 0; i < n; i++)
		list[i]->normalized_zpos = i;
}

static int dpy_check_planes(struct dpy_atomic_state *s)
{
	struct dpy_atomic_plane *ap;
	struct dpy_atomic_crtc *ac;
	struct dpy_crtc_state *cs;
	unsigned int i;
	int ret;

	/* crtcs losing or gaining planes are part of the update */
	dpy_for_each_plane_in_state(s, i, ap) {
		if (ap->old_state->crtc &&
		    !dpy_atomic_get_crtc_state(s, ap->old_state->crtc))
			return -ENOMEM;
		if (ap->new_state->crtc &&
		    !dpy_atomic_get_crtc_state(s, ap->new_state->crtc))
			return -ENOMEM;
	}
	/* hardware constraints are per crtc: check all its planes together */
	dpy_for_each_crtc_in_state(s, i, ac) {
		ret = dpy_atomic_add_affected_planes(s, ac->ptr);
		if (ret)
			return ret;
	}

	dpy_for_each_plane_in_state(s, i, ap) {
		struct dpy_plane *plane = ap->ptr;
		struct dpy_plane_state *new = ap->new_state;
		struct dpy_plane_state *old = ap->old_state;

		if (new->crtc) {
			if (!(plane->possible_crtcs & DPY_BIT(new->crtc->index))) {
				dpy_err("%s cannot be used on %s\n",
					plane->name, new->crtc->name);
				return -EINVAL;
			}
			/* a blanked (enabled, inactive) crtc keeps its planes */
			cs = dpy_atomic_new_crtc_state(s, new->crtc);
			if (!cs->enable) {
				dpy_err("%s: crtc %s is off\n", plane->name,
					new->crtc->name);
				return -EINVAL;
			}
			if (!new->fb.info ||
			    !dpy_plane_has_format(plane, new->fb.format)) {
				dpy_err("%s: format %s unsupported\n",
					plane->name,
					display_format_name(new->fb.format));
				return -EINVAL;
			}
		} else {
			new->visible = false;
		}

		if (memcmp(old, new, sizeof(*new))) {
			if (old->crtc)
				dpy_atomic_new_crtc_state(s, old->crtc)->planes_changed = true;
			if (new->crtc)
				dpy_atomic_new_crtc_state(s, new->crtc)->planes_changed = true;
		}
	}

	dpy_for_each_crtc_in_state(s, i, ac) {
		dpy_normalize_zpos(s, ac->ptr);
		dpy_update_crtc_masks(s, ac->new_state);
	}

	dpy_for_each_plane_in_state(s, i, ap) {
		if (!ap->ptr->funcs->atomic_check)
			continue;
		ret = ap->ptr->funcs->atomic_check(ap->ptr, s);
		if (ret) {
			dpy_dbg("%s: check failed: %d\n", ap->ptr->name, ret);
			return ret;
		}
	}
	return 0;
}

int dpy_atomic_check(struct dpy_atomic_state *s)
{
	struct dpy_atomic_connector *acon;
	struct dpy_atomic_crtc *ac;
	unsigned int i;
	int ret;

	ret = dpy_check_connectors(s);
	if (ret)
		return ret;
	ret = dpy_check_crtcs_modeset(s);
	if (ret)
		return ret;
	ret = dpy_add_affected_connectors(s);
	if (ret)
		return ret;
	ret = dpy_check_encoders(s);
	if (ret)
		return ret;
	ret = dpy_check_planes(s);
	if (ret)
		return ret;

	dpy_for_each_crtc_in_state(s, i, ac) {
		if (!ac->ptr->funcs->atomic_check)
			continue;
		ret = ac->ptr->funcs->atomic_check(ac->ptr, s);
		if (ret) {
			dpy_dbg("%s: check failed: %d\n", ac->ptr->name, ret);
			return ret;
		}
	}

	dpy_for_each_connector_in_state(s, i, acon) {
		if (!acon->ptr->funcs || !acon->ptr->funcs->atomic_check)
			continue;
		ret = acon->ptr->funcs->atomic_check(acon->ptr, s);
		if (ret)
			return ret;
	}

	s->checked = true;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Commit                                                              */
/* ------------------------------------------------------------------ */
static void dpy_swap_state(struct dpy_atomic_state *s)
{
	struct dpy_atomic_connector *acon;
	struct dpy_atomic_plane *ap;
	struct dpy_atomic_crtc *ac;
	unsigned int i;

	dpy_for_each_plane_in_state(s, i, ap)
		ap->ptr->state = ap->new_state;
	dpy_for_each_crtc_in_state(s, i, ac)
		ac->ptr->state = ac->new_state;
	dpy_for_each_connector_in_state(s, i, acon)
		acon->ptr->state = acon->new_state;
	s->swapped = true;
}

static void dpy_commit_disables(struct dpy_atomic_state *s)
{
	struct dpy_atomic_connector *acon;
	struct dpy_atomic_crtc *ac;
	unsigned int i, j;

	dpy_for_each_crtc_in_state(s, i, ac) {
		struct dpy_crtc_state *old = ac->old_state;
		struct dpy_crtc_state *new = ac->new_state;
		struct dpy_crtc *crtc = ac->ptr;

		if (!old->active || (new->active && !dpy_crtc_needs_modeset(new)))
			continue;

		dpy_crtc_vblank_put(crtc);

		dpy_for_each_connector_in_state(s, j, acon) {
			struct dpy_encoder *enc = acon->old_state->encoder;

			if (acon->old_state->crtc != crtc || !enc)
				continue;
			if (enc->funcs && enc->funcs->atomic_disable)
				enc->funcs->atomic_disable(enc, s);
			enc->crtc = NULL;
		}
		/* connectors not part of this update keep their encoder */
		if (crtc->funcs->atomic_disable)
			crtc->funcs->atomic_disable(crtc, s);
	}
}

static void dpy_commit_enables(struct dpy_atomic_state *s)
{
	struct dpy_atomic_connector *acon;
	struct dpy_atomic_crtc *ac;
	unsigned int i, j;

	dpy_for_each_crtc_in_state(s, i, ac) {
		struct dpy_crtc_state *old = ac->old_state;
		struct dpy_crtc_state *new = ac->new_state;
		struct dpy_crtc *crtc = ac->ptr;

		if (!new->active || (old->active && !dpy_crtc_needs_modeset(new)))
			continue;

		dpy_for_each_connector_in_state(s, j, acon) {
			struct dpy_encoder *enc = acon->new_state->encoder;

			if (acon->new_state->crtc != crtc || !enc)
				continue;
			if (enc->funcs && enc->funcs->atomic_mode_set)
				enc->funcs->atomic_mode_set(enc, new,
							    acon->new_state);
		}

		if (crtc->funcs->atomic_enable)
			crtc->funcs->atomic_enable(crtc, s);

		dpy_for_each_connector_in_state(s, j, acon) {
			struct dpy_encoder *enc = acon->new_state->encoder;

			if (acon->new_state->crtc != crtc || !enc)
				continue;
			enc->crtc = crtc;
			if (enc->funcs && enc->funcs->atomic_enable)
				enc->funcs->atomic_enable(enc, s);
		}

		dpy_crtc_vblank_get(crtc);
	}
}

static void dpy_commit_planes(struct dpy_atomic_state *s)
{
	struct dpy_atomic_plane *ap;
	struct dpy_atomic_crtc *ac;
	unsigned int i, j;

	dpy_for_each_crtc_in_state(s, i, ac) {
		struct dpy_crtc *crtc = ac->ptr;

		if (!ac->new_state->active)
			continue;

		if (crtc->funcs->atomic_begin)
			crtc->funcs->atomic_begin(crtc, s);

		dpy_for_each_plane_in_state(s, j, ap) {
			struct dpy_plane *plane = ap->ptr;

			if (ap->new_state->crtc == crtc &&
			    ap->new_state->visible) {
				if (plane->funcs->atomic_update)
					plane->funcs->atomic_update(plane, s);
			} else if (ap->old_state->crtc == crtc) {
				if (plane->funcs->atomic_disable)
					plane->funcs->atomic_disable(plane, s);
			}
		}

		if (crtc->funcs->atomic_flush)
			crtc->funcs->atomic_flush(crtc, s);
	}
}

static void dpy_commit_wait(struct dpy_atomic_state *s)
{
	struct dpy_atomic_crtc *ac;
	uint64_t t0;
	unsigned int i;

	dpy_for_each_crtc_in_state(s, i, ac) {
		struct dpy_crtc *crtc = ac->ptr;

		if (!ac->new_state->active || !ac->new_state->flip_needed)
			continue;
		t0 = dpy_os_time_us();
		if (dpy_crtc_wait_flip(crtc, DPY_FLIP_TIMEOUT_MS))
			dpy_warn("%s: flip timed out\n", crtc->name);
		crtc->last_commit_us = (uint32_t)(dpy_os_time_us() - t0);
	}
}

/* consumes @s; the device lock taken at allocation is released here */
int dpy_atomic_commit(struct dpy_atomic_state *s, uint32_t flags)
{
	struct dpy_atomic_crtc *ac;
	unsigned int i;
	int ret = 0;

	s->flags |= flags;
	if (!s->checked)
		ret = dpy_atomic_check(s);
	if (ret || (s->flags & DPY_COMMIT_TEST_ONLY))
		goto out;

	dpy_swap_state(s);

	dpy_commit_disables(s);
	dpy_commit_enables(s);
	dpy_commit_planes(s);
	if (!(s->flags & DPY_COMMIT_NONBLOCK))
		dpy_commit_wait(s);

	dpy_for_each_crtc_in_state(s, i, ac)
		ac->ptr->commit_count++;

out:
	dpy_atomic_state_free(s);
	return ret;
}

int dpy_atomic_disable_all(struct dpy_device *dev)
{
	struct dpy_atomic_state *s;
	struct dpy_atomic_plane *ap;
	struct dpy_connector *conn;
	struct dpy_crtc *crtc;
	unsigned int i;

	s = dpy_atomic_state_alloc(dev);
	if (!s)
		return -ENOMEM;

	dpy_list_for_each_entry(crtc, &dev->crtcs, head) {
		struct dpy_crtc_state *cs = dpy_atomic_get_crtc_state(s, crtc);

		if (!cs || dpy_atomic_add_affected_planes(s, crtc))
			goto nomem;
		cs->active = false;
		cs->enable = false;
	}
	dpy_list_for_each_entry(conn, &dev->connectors, head) {
		struct dpy_connector_state *st;

		st = dpy_atomic_get_connector_state(s, conn);
		if (!st)
			goto nomem;
		st->crtc = NULL;
	}
	dpy_for_each_plane_in_state(s, i, ap)
		ap->new_state->crtc = NULL;

	return dpy_atomic_commit(s, DPY_COMMIT_ALLOW_MODESET);

nomem:
	dpy_atomic_state_free(s);
	return -ENOMEM;
}

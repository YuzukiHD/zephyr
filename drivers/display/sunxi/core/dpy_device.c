// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Display pipeline framework - driver probing, component binding and the
 * display device life cycle.
 */
#define DPY_LOG_TAG "core"
#include <dpy/dpy_device.h>
#include <dpy/dpy_kms.h>
#include <dpy/dpy_log.h>

#if defined(CONFIG_DISP2_SUNXI)
#error "display-engine and the legacy disp2 driver are mutually exclusive"
#endif

static struct dpy_list dpy_dev_list = DPY_LIST_INIT(dpy_dev_list);

/* the component master: every component needed by the pipelines */
static struct {
	struct dpy_dev *match[DPY_GRAPH_MAX_NODES];
	unsigned int num_match;
	bool bound;
	struct dpy_device *ddev;
	/* posted once the boot modeset is done (or probing failed) */
	struct dpy_sem *ready_sem;
	int ready_err;
} dpy_master;

/* ------------------------------------------------------------------ */
/* Resources                                                           */
/* ------------------------------------------------------------------ */
uintptr_t dpy_dev_ioremap(struct dpy_dev *dev, const char *name, size_t *size)
{
	const struct dpy_res *r;

	r = dpy_node_get_res(dev->node, DPY_RES_MMIO, name);
	if (!r) {
		dpy_err("%s: no register window %s\n", dpy_dev_name(dev),
			name ? name : "");
		return 0;
	}
	if (size)
		*size = r->mmio.size;
	return dpy_os_ioremap(r->mmio.base, r->mmio.size);
}

int dpy_dev_irq(struct dpy_dev *dev, const char *name)
{
	const struct dpy_res *r;

	r = dpy_node_get_res(dev->node, DPY_RES_IRQ, name);
	return r ? r->irq.num : -ENOENT;
}

struct dpy_clk *dpy_dev_clk_get(struct dpy_dev *dev, const char *name)
{
	const struct dpy_res *r;
	struct dpy_clk *clk;

	r = dpy_node_get_res(dev->node, DPY_RES_CLK, name);
	if (!r)
		return NULL;
	clk = dpy_os_clk_get(r->clk.ctrl, r->clk.id);
	if (!clk)
		dpy_err("%s: clock %s (%u:%u) unavailable\n",
			dpy_dev_name(dev), name, r->clk.ctrl, r->clk.id);
	return clk;
}

int dpy_dev_clk_setup(struct dpy_dev *dev, const char *name,
		      struct dpy_clk *clk)
{
	const struct dpy_res *r;
	struct dpy_clk *parent;
	int ret = 0;

	r = dpy_node_get_res(dev->node, DPY_RES_CLK, name);
	if (!r || !clk)
		return -ENOENT;

	if (r->clk.parent != DPY_CLK_NO_PARENT) {
		parent = dpy_os_clk_get(r->clk.ctrl, r->clk.parent);
		if (parent) {
			ret = dpy_os_clk_set_parent(clk, parent);
			dpy_os_clk_put(parent);
		}
		if (ret)
			dpy_warn("%s: cannot reparent clock %s\n",
				 dpy_dev_name(dev), name);
	}
	if (r->clk.rate) {
		ret = dpy_os_clk_set_rate(clk, r->clk.rate);
		if (ret)
			dpy_warn("%s: cannot set %s to %u Hz\n",
				 dpy_dev_name(dev), name, r->clk.rate);
	}
	return ret;
}

struct dpy_reset *dpy_dev_reset_get(struct dpy_dev *dev, const char *name)
{
	const struct dpy_res *r;
	struct dpy_reset *rst;

	r = dpy_node_get_res(dev->node, DPY_RES_RST, name);
	if (!r)
		return NULL;
	rst = dpy_os_reset_get(r->rst.ctrl, r->rst.id);
	if (!rst)
		dpy_err("%s: reset %s (%u:%u) unavailable\n",
			dpy_dev_name(dev), name, r->rst.ctrl, r->rst.id);
	return rst;
}

/* ------------------------------------------------------------------ */
/* Driver instances                                                    */
/* ------------------------------------------------------------------ */
struct dpy_dev *dpy_dev_find(const char *node_name)
{
	struct dpy_dev *dev;

	dpy_list_for_each_entry(dev, &dpy_dev_list, list)
		if (!strcmp(dpy_dev_name(dev), node_name))
			return dev;
	return NULL;
}

struct dpy_dev *dpy_dev_from_node(const struct dpy_gnode *node)
{
	return node ? node->dev : NULL;
}

/*
 * The node lists its compatibles most specific first; the first one any
 * driver handles wins, so a SoC specific entry overrides the family one.
 */
static const struct dpy_driver *dpy_match_driver(const char *const *compatible,
						 const void **data)
{
	const struct dpy_driver *drv;
	const struct dpy_match *m;
	unsigned int i, j;

	if (!compatible)
		return NULL;
	for (j = 0; compatible[j]; j++) {
		for (i = 0; i < dpy_driver_table_size; i++) {
			drv = dpy_driver_table[i];
			for (m = drv->match; m && m->compatible; m++) {
				if (strcmp(m->compatible, compatible[j]))
					continue;
				*data = m->data;
				return drv;
			}
		}
	}
	return NULL;
}

static int dpy_probe_node(struct dpy_gnode *node)
{
	const struct dpy_driver *drv;
	const void *data = NULL;
	struct dpy_dev *dev;
	int ret;

	drv = dpy_match_driver(node->desc->compatible, &data);
	if (!drv) {
		dpy_err("%s: no driver for \"%s\" (disabled in Kconfig?)\n",
			node->desc->name, node->desc->compatible[0]);
		return -ENODEV;
	}

	dev = dpy_os_zalloc(sizeof(*dev));
	if (!dev)
		return -ENOMEM;
	dev->node = node;
	dev->drv = drv;
	dev->match_data = data;
	dpy_list_init(&dev->list);
	node->dev = dev;
	dpy_list_add_tail(&dev->list, &dpy_dev_list);

	ret = drv->probe ? drv->probe(dev) : 0;
	if (ret) {
		dpy_err("%s: probe with %s failed: %d\n", dpy_dev_name(dev),
			drv->name, ret);
		dpy_list_del(&dev->list);
		node->dev = NULL;
		dpy_os_free(dev);
		return ret;
	}
	dpy_dbg("%s: probed by %s\n", dpy_dev_name(dev), drv->name);
	return 0;
}

static void dpy_remove_all(void)
{
	struct dpy_dev *dev, *tmp;

	dpy_list_for_each_entry_safe(dev, tmp, &dpy_dev_list, list) {
		if (dev->drv->remove)
			dev->drv->remove(dev);
		dpy_list_del(&dev->list);
		dev->node->dev = NULL;
		dpy_os_free(dev);
	}
}

/* ------------------------------------------------------------------ */
/* Component master                                                    */
/* ------------------------------------------------------------------ */
static bool dpy_match_contains(const struct dpy_gnode *node)
{
	unsigned int i;

	for (i = 0; i < dpy_master.num_match; i++)
		if (dpy_master.match[i]->node == node)
			return true;
	return false;
}

/*
 * Walk the graph from @node over links and references and add every
 * reachable enabled node to the match list. Returns -ENODEV when a node
 * of a pipeline has no probed driver.
 */
static int dpy_match_walk(struct dpy_gnode *node)
{
	const struct dpy_node *d = node->desc;
	struct dpy_gnode *peer;
	unsigned int port, iter, i;
	int ret;

	if (dpy_match_contains(node))
		return 0;
	if (!node->dev) {
		dpy_err("pipeline node %s has no driver\n", d->name);
		return -ENODEV;
	}
	if (dpy_master.num_match >= DPY_GRAPH_MAX_NODES)
		return -ENOSPC;
	dpy_master.match[dpy_master.num_match++] = node->dev;

	/* ports are small integers; 4 covers every block we know */
	for (port = 0; port < 4; port++) {
		iter = 0;
		while ((peer = dpy_node_remote_next(node, port, &iter, NULL,
						    NULL))) {
			ret = dpy_match_walk(peer);
			if (ret)
				return ret;
		}
	}
	for (i = 0; i < d->nrefs; i++) {
		peer = dpy_node_get_ref(node, d->refs[i].name);
		if (!peer)
			continue;
		ret = dpy_match_walk(peer);
		if (ret)
			return ret;
	}
	return 0;
}

static int dpy_build_match(void)
{
	struct dpy_dev *dev;
	int ret;

	dpy_master.num_match = 0;
	dpy_list_for_each_entry(dev, &dpy_dev_list, list) {
		if (dev->drv->klass != DPY_COMP_ENGINE)
			continue;
		ret = dpy_match_walk(dev->node);
		if (ret)
			return ret;
	}
	if (!dpy_master.num_match) {
		dpy_err("no display engine enabled\n");
		return -ENODEV;
	}
	return 0;
}

static void dpy_unbind_all(struct dpy_device *ddev)
{
	int klass;
	int i;

	for (klass = DPY_COMP_NR - 1; klass >= 0; klass--) {
		for (i = (int)dpy_master.num_match - 1; i >= 0; i--) {
			struct dpy_dev *dev = dpy_master.match[i];

			if ((int)dev->drv->klass != klass || !dev->bound)
				continue;
			if (dev->drv->ops && dev->drv->ops->unbind)
				dev->drv->ops->unbind(dev, ddev);
			dev->bound = false;
		}
	}
}

static int dpy_bind_all(struct dpy_device *ddev)
{
	unsigned int klass, i;
	int ret;

	for (klass = 0; klass < DPY_COMP_NR; klass++) {
		for (i = 0; i < dpy_master.num_match; i++) {
			struct dpy_dev *dev = dpy_master.match[i];

			if (dev->drv->klass != klass)
				continue;
			ret = 0;
			if (dev->drv->ops && dev->drv->ops->bind)
				ret = dev->drv->ops->bind(dev, ddev);
			if (ret) {
				dpy_err("%s: bind failed: %d\n",
					dpy_dev_name(dev), ret);
				dpy_unbind_all(ddev);
				return ret;
			}
			dev->bound = true;
			dpy_info("bound %s (%s)\n", dpy_dev_name(dev),
				 dev->drv->name);
		}
	}
	return 0;
}

static struct dpy_device *dpy_device_alloc(void)
{
	struct dpy_device *ddev;

	ddev = dpy_os_zalloc(sizeof(*ddev));
	if (!ddev)
		return NULL;
	dpy_list_init(&ddev->planes);
	dpy_list_init(&ddev->crtcs);
	dpy_list_init(&ddev->encoders);
	dpy_list_init(&ddev->connectors);
	ddev->max_width = 4096;
	ddev->max_height = 4096;
	ddev->lock = dpy_os_mutex_create();
	if (!ddev->lock) {
		dpy_os_free(ddev);
		return NULL;
	}
	return ddev;
}

static void dpy_device_free(struct dpy_device *ddev)
{
	dpy_os_mutex_destroy(ddev->lock);
	dpy_os_free(ddev);
}

static void dpy_master_set_ready(int err)
{
	dpy_master.ready_err = err;
	dpy_os_sem_post(dpy_master.ready_sem);
}

/*
 * Light up every connected output with its preferred mode. Planes stay
 * disabled, so the screen shows the background colour until a client
 * commits a framebuffer.
 */
static int dpy_device_boot_modeset(struct dpy_device *ddev)
{
	struct dpy_atomic_state *s;
	struct dpy_connector *conn;
	struct dpy_connector_state *conn_state;
	struct dpy_crtc_state *crtc_state;
	struct dpy_crtc *crtc;
	uint32_t used_crtcs = 0;
	int ret;

	s = dpy_atomic_state_alloc(ddev);
	if (!s)
		return -ENOMEM;

	dpy_list_for_each_entry(conn, &ddev->connectors, head) {
		if (conn->type == DPY_CONNECTOR_WRITEBACK || !conn->encoder)
			continue;
		dpy_connector_update_modes(conn);
		if (!conn->num_modes) {
			dpy_warn("%s: no modes, output left off\n", conn->name);
			continue;
		}

		crtc = NULL;
		dpy_list_for_each_entry(crtc, &ddev->crtcs, head)
			if ((conn->encoder->possible_crtcs & DPY_BIT(crtc->index)) &&
			    !(used_crtcs & DPY_BIT(crtc->index)))
				break;
		if (&crtc->head == &ddev->crtcs) {
			dpy_warn("%s: no free crtc\n", conn->name);
			continue;
		}
		used_crtcs |= DPY_BIT(crtc->index);

		conn_state = dpy_atomic_get_connector_state(s, conn);
		crtc_state = dpy_atomic_get_crtc_state(s, crtc);
		if (!conn_state || !crtc_state) {
			dpy_atomic_state_free(s);
			return -ENOMEM;
		}
		conn_state->crtc = crtc;
		conn_state->encoder = conn->encoder;
		crtc_state->enable = true;
		crtc_state->active = true;
		crtc_state->mode = conn->modes[conn->preferred];
		dpy_info("%s -> %s -> %s: %ux%u@%u.%03uHz\n", crtc->name,
			 conn->encoder->name, conn->name,
			 crtc_state->mode.hdisplay, crtc_state->mode.vdisplay,
			 dpy_mode_vrefresh_mhz(&crtc_state->mode) / 1000,
			 dpy_mode_vrefresh_mhz(&crtc_state->mode) % 1000);
	}

	ret = dpy_atomic_commit(s, DPY_COMMIT_ALLOW_MODESET);
	if (ret)
		dpy_err("boot modeset failed: %d\n", ret);
	return ret;
}

static int dpy_master_try_bind(void)
{
	struct dpy_device *ddev;
	unsigned int i;
	int ret;

	if (dpy_master.bound)
		return 0;
	for (i = 0; i < dpy_master.num_match; i++)
		if (!dpy_master.match[i]->component_added)
			return -EAGAIN;

	ddev = dpy_device_alloc();
	if (!ddev)
		return -ENOMEM;

	ret = dpy_bind_all(ddev);
	if (ret) {
		dpy_device_free(ddev);
		return ret;
	}

	dpy_master.bound = true;
	dpy_master.ddev = ddev;
	ddev->registered = true;

	dpy_master_set_ready(dpy_device_boot_modeset(ddev));
	return 0;
}

int dpy_component_add(struct dpy_dev *dev)
{
	int ret;

	dev->component_added = true;
	/* a late addition may complete the match list */
	if (!dpy_master.num_match || !dpy_match_contains(dev->node))
		return 0;
	ret = dpy_master_try_bind();
	return ret == -EAGAIN ? 0 : ret;
}

void dpy_component_del(struct dpy_dev *dev)
{
	dev->component_added = false;
}

struct dpy_device *dpy_device_get(void)
{
	return dpy_master.bound ? dpy_master.ddev : NULL;
}

int dpy_core_wait_ready(uint32_t timeout_ms)
{
	int ret;

	if (!dpy_master.ready_sem)
		return -ENODEV;
	ret = dpy_os_sem_wait(dpy_master.ready_sem, timeout_ms);
	if (ret)
		return ret;
	/* let the other waiters through as well */
	dpy_os_sem_post(dpy_master.ready_sem);
	return dpy_master.ready_err;
}

int dpy_core_probe(const struct dpy_soc_desc *soc,
		   const struct dpy_board_desc *board)
{
	unsigned int i;
	int ret;

	if (dpy_master.bound)
		return -EBUSY;
	if (!dpy_master.ready_sem) {
		/* kept for the lifetime of the system, waiters may hold it */
		dpy_master.ready_sem = dpy_os_sem_create(0);
		if (!dpy_master.ready_sem)
			return -ENOMEM;
	}
	/* drop the result of an earlier, failed probe */
	while (!dpy_os_sem_wait(dpy_master.ready_sem, 0))
		;
	dpy_master.num_match = 0;

	ret = dpy_graph_load(soc, board);
	if (ret) {
		dpy_err("invalid hardware description: %d\n", ret);
		dpy_master_set_ready(ret);
		return ret;
	}
	dpy_info("soc %s, board %s\n", soc->name, board ? board->name : "-");

	if (soc->init) {
		ret = soc->init();
		if (ret)
			goto err;
	}

	for (i = 0; i < dpy_graph_num_nodes(); i++) {
		struct dpy_gnode *node = dpy_graph_node(i);

		if (!dpy_node_is_okay(node) || !node->desc->compatible)
			continue;
		ret = dpy_probe_node(node);
		if (ret)
			dpy_warn("%s left unprobed\n", node->desc->name);
	}

	ret = dpy_build_match();
	if (ret)
		goto err;

	ret = dpy_master_try_bind();
	if (ret == -EAGAIN) {
		dpy_info("waiting for components\n");
		return 0;
	}
	if (ret)
		goto err;
	return 0;

err:
	/* the match list points at the devices freed here */
	dpy_master.num_match = 0;
	dpy_remove_all();
	dpy_graph_reset();
	dpy_master_set_ready(ret);
	return ret;
}

void dpy_core_remove(void)
{
	struct dpy_device *ddev = dpy_master.ddev;

	if (ddev) {
		/* switch every output off before tearing down */
		dpy_atomic_disable_all(ddev);
		dpy_unbind_all(ddev);
		dpy_device_free(ddev);
	}
	dpy_master.bound = false;
	dpy_master.ddev = NULL;
	dpy_master.num_match = 0;
	dpy_remove_all();
	dpy_graph_reset();
	/* not ready any more: consume the pending ready token */
	while (dpy_master.ready_sem && !dpy_os_sem_wait(dpy_master.ready_sem, 0))
		;
}

/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later */
/*
 * Display pipeline framework - drivers, components and the display device.
 *
 * Probing and binding follow the Linux component framework:
 *
 *  1. every enabled graph node is matched against the driver table by its
 *     compatible string and the driver's probe() runs; probe only collects
 *     resources and allocates private data, it never touches hardware;
 *  2. a probed driver registers itself as a component (dpy_component_add);
 *     a driver may delay this until it is ready (e.g. a DSI host waiting
 *     for its panel to attach);
 *  3. the display master walks the graph starting at every display engine
 *     node and builds the list of components that form complete pipelines;
 *  4. once all of them are registered the master binds them in class
 *     order (TOP, ENGINE, TCON, PHY, BACKLIGHT, PANEL, BRIDGE, ENCODER).
 *     bind() creates the KMS objects (CRTCs, planes, encoders, ...) and
 *     wires them to their neighbours found through the graph;
 *  5. the device is registered and the boot modeset lights the display.
 */
#ifndef __DPY_DEVICE_H__
#define __DPY_DEVICE_H__

#include <dpy/dpy_graph.h>
#include <dpy/dpy_os.h>

struct dpy_device;
struct dpy_dev;

enum dpy_comp_class {
	DPY_COMP_TOP = 0,	/* shared glue blocks (tcon top, ...) */
	DPY_COMP_ENGINE,	/* display engine: CRTCs and planes */
	DPY_COMP_TCON,		/* timing controllers */
	DPY_COMP_PHY,
	DPY_COMP_BACKLIGHT,
	DPY_COMP_PANEL,
	DPY_COMP_BRIDGE,
	DPY_COMP_ENCODER,	/* interfaces: rgb, lvds, dsi */
	DPY_COMP_NR,
};

struct dpy_component_ops {
	int (*bind)(struct dpy_dev *dev, struct dpy_device *ddev);
	void (*unbind)(struct dpy_dev *dev, struct dpy_device *ddev);
};

/* one compatible a driver handles, with the per-SoC data that goes with it */
struct dpy_match {
	const char *compatible;
	const void *data;
};

struct dpy_driver {
	const char *name;
	const struct dpy_match *match;	/* terminated by an empty entry */
	enum dpy_comp_class klass;
	int (*probe)(struct dpy_dev *dev);
	void (*remove)(struct dpy_dev *dev);
	const struct dpy_component_ops *ops;
};

/* a driver instance bound to one graph node */
struct dpy_dev {
	struct dpy_gnode *node;
	const struct dpy_driver *drv;
	const void *match_data;		/* dpy_match.data of the matched entry */
	void *priv;
	bool component_added;
	bool bound;
	struct dpy_list list;		/* dpy_dev_list */
};

static inline const char *dpy_dev_name(const struct dpy_dev *dev)
{
	return dev->node->desc->name;
}

static inline const void *dpy_dev_pdata(const struct dpy_dev *dev)
{
	return dev->node->pdata;
}

/* per-SoC (variant) data of the compatible this device matched */
static inline const void *dpy_dev_match_data(const struct dpy_dev *dev)
{
	return dev->match_data;
}

/* ---- driver table (dpy_driver_table.c) ---- */
extern const struct dpy_driver *const dpy_driver_table[];
extern const unsigned int dpy_driver_table_size;

/* ---- device instances ---- */
struct dpy_dev *dpy_dev_find(const char *node_name);
/* driver instance probed for @node, NULL if not probed/enabled */
struct dpy_dev *dpy_dev_from_node(const struct dpy_gnode *node);

int dpy_component_add(struct dpy_dev *dev);
void dpy_component_del(struct dpy_dev *dev);

/* ---- resources helpers ---- */
uintptr_t dpy_dev_ioremap(struct dpy_dev *dev, const char *name,
			  size_t *size);
int dpy_dev_irq(struct dpy_dev *dev, const char *name);
struct dpy_clk *dpy_dev_clk_get(struct dpy_dev *dev, const char *name);
/* apply the parent and rate given by the clock resource */
int dpy_dev_clk_setup(struct dpy_dev *dev, const char *name,
		      struct dpy_clk *clk);
struct dpy_reset *dpy_dev_reset_get(struct dpy_dev *dev, const char *name);

/* ---- top level ---- */
int dpy_core_probe(const struct dpy_soc_desc *soc,
		   const struct dpy_board_desc *board);
void dpy_core_remove(void);

/* the (single) display device once bound, NULL before */
struct dpy_device *dpy_device_get(void);
/* wait until the boot modeset finished; returns its result */
int dpy_core_wait_ready(uint32_t timeout_ms);

#endif /* __DPY_DEVICE_H__ */

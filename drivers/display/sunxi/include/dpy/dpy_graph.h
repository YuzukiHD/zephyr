/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later */
/*
 * Display pipeline framework - hardware description graph.
 *
 * The graph plays the role a devicetree plays on Linux:
 *  - nodes describe hardware blocks (register window, IRQ, clocks, resets)
 *    and carry a compatible string used to match a driver;
 *  - links connect node ports (DE output -> TCON input -> encoder -> panel),
 *    the equivalent of of_graph port/endpoint pairs;
 *  - refs are named pointers to other nodes (phandle equivalents), e.g. a
 *    TCON references its "top" block, an encoder its "phy".
 *
 * A SoC description (soc/<soc>.c) declares the on-chip nodes and links, a
 * board description (boards/<board>.c) enables the nodes the board uses,
 * attaches board data (panel timings, pins, ...) and adds off-chip nodes
 * such as panels and backlights.
 */
#ifndef __DPY_GRAPH_H__
#define __DPY_GRAPH_H__

#include <dpy/dpy_types.h>

enum dpy_res_type {
	DPY_RES_MMIO,
	DPY_RES_IRQ,
	DPY_RES_CLK,
	DPY_RES_RST,
};

#define DPY_CLK_NO_PARENT	0U

struct dpy_res {
	uint8_t type;
	const char *name;
	union {
		struct {
			uintptr_t base;
			uint32_t size;
		} mmio;
		struct {
			int num;
		} irq;
		struct {
			uint16_t ctrl;
			uint32_t id;
			uint32_t parent;	/* DPY_CLK_NO_PARENT if none */
			uint32_t rate;		/* 0: leave untouched */
		} clk;
		struct {
			uint16_t ctrl;
			uint32_t id;
		} rst;
	};
};

#define DPY_RES_MMIO(_name, _base, _size) \
	{ .type = DPY_RES_MMIO, .name = _name, .mmio = { _base, _size } }
#define DPY_RES_IRQ(_name, _num) \
	{ .type = DPY_RES_IRQ, .name = _name, .irq = { _num } }
#define DPY_RES_CLK(_name, _ctrl, _id, _parent, _rate) \
	{ .type = DPY_RES_CLK, .name = _name, .clk = { _ctrl, _id, _parent, _rate } }
#define DPY_RES_RST(_name, _ctrl, _id) \
	{ .type = DPY_RES_RST, .name = _name, .rst = { _ctrl, _id } }

struct dpy_ref {
	const char *name;
	const char *target;
};

enum dpy_node_status {
	DPY_STATUS_DISABLED = 0,
	DPY_STATUS_OKAY,
};

/*
 * Compatible list of a node, most specific first, e.g.
 * DPY_COMPATIBLE("allwinner,sun252iw2-tcon-lcd", "allwinner,sunxi-tcon-lcd").
 * A driver matches on the first entry any driver claims.
 */
#define DPY_COMPATIBLE(...) ((const char *const[]){ __VA_ARGS__, NULL })

struct dpy_node {
	const char *name;
	const char *const *compatible;	/* NULL terminated list */
	uint8_t status;
	uint8_t id;			/* hardware instance number */
	const struct dpy_res *res;
	uint8_t nres;
	const struct dpy_ref *refs;
	uint8_t nrefs;
	const void *pdata;
};

/* an undirected connection between two node ports */
struct dpy_link {
	const char *a;
	uint8_t a_port;
	uint8_t a_ep;
	const char *b;
	uint8_t b_port;
	uint8_t b_ep;
};

/* board specific settings applied to a SoC node */
struct dpy_node_override {
	const char *name;
	uint8_t status;
	const void *pdata;
};

struct dpy_soc_desc {
	const char *name;
	const struct dpy_node *nodes;
	uint8_t nnodes;
	const struct dpy_link *links;
	uint8_t nlinks;
	/* optional one-time SoC setup (bus/SRAM routing, calibration) */
	int (*init)(void);
};

struct dpy_board_desc {
	const char *name;
	const struct dpy_node *nodes;
	uint8_t nnodes;
	const struct dpy_link *links;
	uint8_t nlinks;
	const struct dpy_node_override *overrides;
	uint8_t noverrides;
};

struct dpy_dev;

/* runtime node: description + board overrides + bound driver instance */
struct dpy_gnode {
	const struct dpy_node *desc;
	uint8_t status;
	const void *pdata;
	struct dpy_dev *dev;
};

#define DPY_GRAPH_MAX_NODES	24
#define DPY_GRAPH_MAX_LINKS	24
#define DPY_EP_ANY		0xff

int dpy_graph_load(const struct dpy_soc_desc *soc,
		   const struct dpy_board_desc *board);
void dpy_graph_reset(void);

unsigned int dpy_graph_num_nodes(void);
struct dpy_gnode *dpy_graph_node(unsigned int index);
struct dpy_gnode *dpy_graph_find(const char *name);

static inline const char *dpy_node_name(const struct dpy_gnode *n)
{
	return n->desc->name;
}

static inline bool dpy_node_is_okay(const struct dpy_gnode *n)
{
	return n && n->status == DPY_STATUS_OKAY;
}

const struct dpy_res *dpy_node_get_res(const struct dpy_gnode *node,
				       enum dpy_res_type type,
				       const char *name);
/* follow a named reference; returns NULL if missing or disabled */
struct dpy_gnode *dpy_node_get_ref(const struct dpy_gnode *node,
				   const char *name);
/*
 * Return the enabled node linked to (@port, @ep) of @node. @ep may be
 * DPY_EP_ANY. When several enabled nodes match, the first is returned.
 * The remote port/endpoint numbers are stored if the pointers are set.
 */
struct dpy_gnode *dpy_node_remote(const struct dpy_gnode *node, uint8_t port,
				  uint8_t ep, uint8_t *remote_port,
				  uint8_t *remote_ep);
/* iterate over all enabled nodes linked to (@port, any ep) */
struct dpy_gnode *dpy_node_remote_next(const struct dpy_gnode *node,
				       uint8_t port, unsigned int *iter,
				       uint8_t *remote_port, uint8_t *remote_ep);

void dpy_graph_dump(void (*print)(const char *fmt, ...));

#endif /* __DPY_GRAPH_H__ */

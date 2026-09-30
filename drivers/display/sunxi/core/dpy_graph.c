// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Display pipeline framework - hardware description graph.
 */
#define DPY_LOG_TAG "graph"
#include <dpy/dpy_graph.h>
#include <dpy/dpy_log.h>

static struct dpy_gnode dpy_nodes[DPY_GRAPH_MAX_NODES];
static unsigned int dpy_num_nodes;

static const struct dpy_link *dpy_links[DPY_GRAPH_MAX_LINKS];
static unsigned int dpy_num_links;

void dpy_graph_reset(void)
{
	memset(dpy_nodes, 0, sizeof(dpy_nodes));
	dpy_num_nodes = 0;
	memset(dpy_links, 0, sizeof(dpy_links));
	dpy_num_links = 0;
}

unsigned int dpy_graph_num_nodes(void)
{
	return dpy_num_nodes;
}

struct dpy_gnode *dpy_graph_node(unsigned int index)
{
	return index < dpy_num_nodes ? &dpy_nodes[index] : NULL;
}

struct dpy_gnode *dpy_graph_find(const char *name)
{
	unsigned int i;

	if (!name)
		return NULL;
	for (i = 0; i < dpy_num_nodes; i++)
		if (!strcmp(dpy_nodes[i].desc->name, name))
			return &dpy_nodes[i];
	return NULL;
}

static int dpy_graph_add_nodes(const struct dpy_node *nodes, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i++) {
		if (dpy_graph_find(nodes[i].name)) {
			dpy_err("duplicate node %s\n", nodes[i].name);
			return -EEXIST;
		}
		if (dpy_num_nodes >= DPY_GRAPH_MAX_NODES) {
			dpy_err("too many nodes\n");
			return -ENOSPC;
		}
		dpy_nodes[dpy_num_nodes].desc = &nodes[i];
		dpy_nodes[dpy_num_nodes].status = nodes[i].status;
		dpy_nodes[dpy_num_nodes].pdata = nodes[i].pdata;
		dpy_num_nodes++;
	}
	return 0;
}

static int dpy_graph_add_links(const struct dpy_link *links, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i++) {
		if (dpy_num_links >= DPY_GRAPH_MAX_LINKS) {
			dpy_err("too many links\n");
			return -ENOSPC;
		}
		dpy_links[dpy_num_links++] = &links[i];
	}
	return 0;
}

static int dpy_graph_validate(void)
{
	unsigned int i, j;

	for (i = 0; i < dpy_num_links; i++) {
		if (!dpy_graph_find(dpy_links[i]->a) ||
		    !dpy_graph_find(dpy_links[i]->b)) {
			dpy_err("link %s:%u <-> %s:%u references unknown node\n",
				dpy_links[i]->a, dpy_links[i]->a_port,
				dpy_links[i]->b, dpy_links[i]->b_port);
			return -EINVAL;
		}
	}

	for (i = 0; i < dpy_num_nodes; i++) {
		const struct dpy_node *d = dpy_nodes[i].desc;

		for (j = 0; j < d->nrefs; j++) {
			if (!dpy_graph_find(d->refs[j].target)) {
				dpy_err("%s: ref %s -> unknown node %s\n",
					d->name, d->refs[j].name,
					d->refs[j].target);
				return -EINVAL;
			}
		}
	}
	return 0;
}

int dpy_graph_load(const struct dpy_soc_desc *soc,
		   const struct dpy_board_desc *board)
{
	unsigned int i;
	int ret;

	dpy_graph_reset();

	ret = dpy_graph_add_nodes(soc->nodes, soc->nnodes);
	if (!ret)
		ret = dpy_graph_add_links(soc->links, soc->nlinks);
	if (!ret && board) {
		ret = dpy_graph_add_nodes(board->nodes, board->nnodes);
		if (!ret)
			ret = dpy_graph_add_links(board->links, board->nlinks);
		for (i = 0; !ret && i < board->noverrides; i++) {
			const struct dpy_node_override *o = &board->overrides[i];
			struct dpy_gnode *n = dpy_graph_find(o->name);

			if (!n) {
				dpy_err("override for unknown node %s\n",
					o->name);
				ret = -EINVAL;
				break;
			}
			n->status = o->status;
			if (o->pdata)
				n->pdata = o->pdata;
		}
	}
	if (!ret)
		ret = dpy_graph_validate();
	if (ret)
		dpy_graph_reset();
	return ret;
}

const struct dpy_res *dpy_node_get_res(const struct dpy_gnode *node,
				       enum dpy_res_type type,
				       const char *name)
{
	const struct dpy_node *d = node->desc;
	unsigned int i;

	for (i = 0; i < d->nres; i++) {
		if (d->res[i].type != type)
			continue;
		if (!name || (d->res[i].name && !strcmp(d->res[i].name, name)))
			return &d->res[i];
	}
	return NULL;
}

struct dpy_gnode *dpy_node_get_ref(const struct dpy_gnode *node,
				   const char *name)
{
	const struct dpy_node *d = node->desc;
	struct dpy_gnode *t;
	unsigned int i;

	for (i = 0; i < d->nrefs; i++) {
		if (strcmp(d->refs[i].name, name))
			continue;
		t = dpy_graph_find(d->refs[i].target);
		return dpy_node_is_okay(t) ? t : NULL;
	}
	return NULL;
}

/* resolve one side of a link seen from @node */
static struct dpy_gnode *dpy_link_peer(const struct dpy_link *l,
				       const struct dpy_gnode *node,
				       uint8_t port, uint8_t ep,
				       uint8_t *rport, uint8_t *rep)
{
	const char *name = node->desc->name;
	struct dpy_gnode *peer = NULL;

	if (!strcmp(l->a, name) && l->a_port == port &&
	    (ep == DPY_EP_ANY || l->a_ep == ep)) {
		peer = dpy_graph_find(l->b);
		if (rport)
			*rport = l->b_port;
		if (rep)
			*rep = l->b_ep;
	} else if (!strcmp(l->b, name) && l->b_port == port &&
		   (ep == DPY_EP_ANY || l->b_ep == ep)) {
		peer = dpy_graph_find(l->a);
		if (rport)
			*rport = l->a_port;
		if (rep)
			*rep = l->a_ep;
	}
	return dpy_node_is_okay(peer) ? peer : NULL;
}

struct dpy_gnode *dpy_node_remote(const struct dpy_gnode *node, uint8_t port,
				  uint8_t ep, uint8_t *remote_port,
				  uint8_t *remote_ep)
{
	struct dpy_gnode *peer;
	unsigned int i;

	for (i = 0; i < dpy_num_links; i++) {
		peer = dpy_link_peer(dpy_links[i], node, port, ep,
				     remote_port, remote_ep);
		if (peer)
			return peer;
	}
	return NULL;
}

struct dpy_gnode *dpy_node_remote_next(const struct dpy_gnode *node,
				       uint8_t port, unsigned int *iter,
				       uint8_t *remote_port, uint8_t *remote_ep)
{
	struct dpy_gnode *peer;

	while (*iter < dpy_num_links) {
		peer = dpy_link_peer(dpy_links[(*iter)++], node, port,
				     DPY_EP_ANY, remote_port, remote_ep);
		if (peer)
			return peer;
	}
	return NULL;
}

void dpy_graph_dump(void (*print)(const char *fmt, ...))
{
	unsigned int i;

	for (i = 0; i < dpy_num_nodes; i++) {
		const struct dpy_gnode *n = &dpy_nodes[i];

		print("  %-12s %-32s %s%s\n", n->desc->name,
		      n->desc->compatible ? n->desc->compatible[0] : "-",
		      dpy_node_is_okay(n) ? "okay" : "disabled",
		      n->dev ? " [bound]" : "");
	}
	for (i = 0; i < dpy_num_links; i++) {
		const struct dpy_link *l = dpy_links[i];

		print("  link %s:%u.%u <-> %s:%u.%u\n", l->a, l->a_port,
		      l->a_ep, l->b, l->b_port, l->b_ep);
	}
}

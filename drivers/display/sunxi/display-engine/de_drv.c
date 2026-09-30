// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Display engine component driver.
 *
 * probe: resources only.
 * bind:  allocate the shadow register arena, build the pipelines from the
 *        SoC description, create one CRTC per backend, one plane per
 *        overlay layer and the write-back connector.
 */
#define DPY_LOG_TAG "de"
#include "de_priv.h"
#include "de_soc.h"

static void de_teardown(struct de_engine *de)
{
	unsigned int d;

	de_wb_destroy(de);
	de_planes_destroy(de);
	for (d = 0; d < de->nbackends; d++)
		if (de->crtc[d].base.dev)
			de_crtc_destroy(de, (uint8_t)d);
	for (d = 0; d < de->npipes; d++) {
		unsigned int i;

		for (i = 0; i < de->pipes[d].nstages; i++)
			dpy_os_free(de->pipes[d].stages[i].priv);
	}
	memset(de->pipes, 0, sizeof(de->pipes));
	memset(de->frontend, 0, sizeof(de->frontend));
	memset(de->backend, 0, sizeof(de->backend));
	memset(de->crtc, 0, sizeof(de->crtc));
	de->npipes = de->nfrontends = de->nbackends = 0;
	dpy_os_dma_free(de->regs.arena);
	memset(&de->regs, 0, sizeof(de->regs));
}

static int de_bind(struct dpy_dev *dev, struct dpy_device *ddev)
{
	struct de_engine *de = dev->priv;
	unsigned int d;
	int ret;

	de->regs.arena = dpy_os_dma_alloc(de->soc->regs_arena, 64, NULL);
	if (!de->regs.arena)
		return -ENOMEM;
	de->regs.arena_size = de->soc->regs_arena;

	ret = de_pipeline_build(de);
	if (ret)
		goto err;

	for (d = 0; d < de->nbackends; d++) {
		ret = de_crtc_create(de, ddev, (uint8_t)d);
		if (ret)
			goto err;
	}
	ret = de_planes_create(de, ddev);
	if (ret)
		goto err;
	ret = de_wb_create(de, ddev);
	if (ret)
		goto err;

	dpy_info("%s: %u channel(s), %u plane(s), %u display(s)\n",
		 de->soc->name, de->nfrontends, de->nplanes, de->nbackends);
	return 0;

err:
	de_teardown(de);
	return ret;
}

static void de_unbind(struct dpy_dev *dev, struct dpy_device *ddev)
{
	(void)ddev;
	de_teardown(dev->priv);
}

static const struct dpy_component_ops de_component_ops = {
	.bind = de_bind,
	.unbind = de_unbind,
};

static int de_probe(struct dpy_dev *dev)
{
	struct de_engine *de;

	de = dpy_os_zalloc(sizeof(*de));
	if (!de)
		return -ENOMEM;
	de->dev = dev;
	de->soc = dpy_dev_match_data(dev);
	de->board = dpy_dev_pdata(dev);
	de->base = dpy_dev_ioremap(dev, "reg", NULL);
	de->mod_clk = dpy_dev_clk_get(dev, "mod");
	de->bus_clk = dpy_dev_clk_get(dev, "bus");
	de->rst = dpy_dev_reset_get(dev, "bus");
	if (!de->soc || !de->base || !de->mod_clk) {
		dpy_os_clk_put(de->mod_clk);
		dpy_os_clk_put(de->bus_clk);
		dpy_os_reset_put(de->rst);
		dpy_os_free(de);
		return -ENODEV;
	}
	dev->priv = de;
	return dpy_component_add(dev);
}

static void de_remove(struct dpy_dev *dev)
{
	struct de_engine *de = dev->priv;

	dpy_component_del(dev);
	dpy_os_clk_put(de->mod_clk);
	dpy_os_clk_put(de->bus_clk);
	dpy_os_reset_put(de->rst);
	dpy_os_free(de);
	dev->priv = NULL;
}

/*
 * The descriptor is the whole pipeline layout of a SoC, so there is no
 * family fallback: every supported SoC gets its own entry.
 */
static const struct dpy_match de_match[] = {
#ifdef CONFIG_DISPLAY_SOC_SUN252IW2
	{ "allwinner,sun252iw2-display-engine", &de_sun252iw2_desc },
#endif
	{ NULL },
};

const struct dpy_driver dpy_de_driver = {
	.name = "display-engine",
	.match = de_match,
	.klass = DPY_COMP_ENGINE,
	.probe = de_probe,
	.remove = de_remove,
	.ops = &de_component_ops,
};

/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Allwinner SPIF: SPI NOR flash controller.
 *
 * One command is described by four registers (phases, flash address, bus
 * widths, counts). A command without data runs from those registers (CPU
 * mode); a command with data is described by a 32 byte descriptor in memory
 * with the same four words and runs through the controller's own DMA. The
 * same four words, written to the registers with the prefetch mode switched
 * on, make the controller issue the read command by itself for every read of
 * the memory window: that is the XIP mapping.
 */

#define DT_DRV_COMPAT allwinner_sunxi_spif

#include <errno.h>
#include <string.h>
#include <zephyr/cache.h>
#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/flash/flash_sunxi_spif.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(flash_sunxi_spif, CONFIG_FLASH_LOG_LEVEL);

/* controller registers */
#define SPIF_VER	0x00
#define SPIF_GC		0x04
#define SPIF_GCA	0x08
#define SPIF_TC		0x0c
#define SPIF_INT_EN	0x14
#define SPIF_INT_STA	0x18
#define SPIF_CSD	0x1c
#define SPIF_PHC	0x20	/* phases of the command */
#define SPIF_TCF	0x24	/* flash address */
#define SPIF_TCS	0x28	/* bus widths and opcodes */
#define SPIF_TNM	0x2c	/* counts */
#define SPIF_PSA	0x34	/* prefetch window start */
#define SPIF_PEA	0x38	/* prefetch window end */
#define SPIF_PMA	0x3c	/* flash address of the window start */
#define SPIF_DMA_CTL	0x40
#define SPIF_DSC	0x44

#define GC_DMA_MODE	BIT(0)
#define GC_ADDR_MAP	BIT(1)
#define GC_NMODE_EN	BIT(2)
#define GC_PMODE_EN	BIT(3)
#define GC_CPHA		BIT(4)
#define GC_CPOL		BIT(5)
#define GC_SS_MASK	GENMASK(7, 6)
#define GC_CS_POL	BIT(8)
#define GC_HOLD_EN	BIT(13)
#define GC_WP_EN	BIT(15)
#define GC_DTR_EN	BIT(16)
#define GC_RX_FBS	BIT(17)
#define GC_TX_FBS	BIT(18)

#define GCA_FIFO_RST	(BIT(0) | BIT(1))
#define GCA_SOFT_RST	BIT(3)
#define GCA_DMA_END	BIT(4)

#define TC_DELAY_MASK	GENMASK(5, 0)
#define TC_DELAY_SW_EN	BIT(6)
#define TC_MODE_MASK	GENMASK(18, 16)
#define TC_SAMPLE_EN	BIT(20)
#define TC_SCKOUT_SEL	BIT(26)

#define INT_ERR		(BIT(8) | BIT(9) | BIT(10))
#define INT_DMA_DONE	BIT(24)

#define CSD_DEFAULT	((5U << 16) | (6U << 8) | 6U)

#define PHC_RX		BIT(8)
#define PHC_TX		BIT(12)
#define PHC_DUMMY	BIT(16)
#define PHC_MODE	BIT(20)
#define PHC_ADDR	BIT(24)
#define PHC_CMD		BIT(28)

#define TCS_DATA_SHIFT	0
#define TCS_MODE_SHIFT	2
#define TCS_MODE_OPCODE_SHIFT 16
#define TCS_ADDR_SHIFT	4
#define TCS_CMD_SHIFT	6
#define TCS_OPCODE_SHIFT 24

#define TNM_DUMMY_SHIFT	16
#define TNM_NORMAL_EN	BIT(28)
#define TNM_LEN_64K	BIT(31)

#define DMA_CTL_START	BIT(0)
#define DMA_CTL_DESC_LEN_MASK	GENMASK(11, 4)
#define DMA_DESC_BYTES	32U

/* descriptor word 0 and 1 */
#define DESC_LAST	BIT(0)
#define DESC_READ	BIT(1)		/* the DMA writes memory */
#define DESC_BURST_INCR16 (7U << 4)
#define DESC_BLOCK_64B	(3U << 24)

/* the controller version from which the address size field is two bits wide */
#define SPIF_VER_V2	0x10002
#define SPIF_VER_V1	0x10001

/* NOR commands */
#define NOR_WRSR	0x01
#define NOR_PP		0x02
#define NOR_RDSR	0x05
#define NOR_WREN	0x06
#define NOR_FAST_READ	0x0b
#define NOR_RDSR2	0x35
#define NOR_WRSR2	0x31
#define NOR_PP_QUAD	0x32
#define NOR_SE		0x20
#define NOR_FAST_READ_QUAD 0x6b
#define NOR_RDID	0x9f
#define NOR_BE64	0xd8
#define DTR_MODE_BYTE	0xff	/* no continuous read */
#define DTR_DUMMY_MAX	12
/* four byte address variants */
#define NOR_FAST_READ_4B 0x0c
#define NOR_PP_4B	0x12
#define NOR_SE_4B	0x21
#define NOR_FAST_READ_QUAD_4B 0x6c
#define NOR_PP_QUAD_4B	0x34
#define NOR_BE64_4B	0xdc

#define SR_WIP		BIT(0)
#define SR_QE_SR1	BIT(6)
#define SR2_QE		BIT(1)
#define SR2_CMP		BIT(6)

#define SECTOR_SIZE	4096U
#define BLOCK_SIZE	65536U
#define PAGE_SIZE_NOR	256U
#define BOUNCE_SIZE	4096U
#define DESC_MAX_LEN	65536U		/* data of one descriptor */
#define DESC_COUNT	16U		/* descriptors of one command */
#define DIRECT_MAX	(DESC_COUNT * DESC_MAX_LEN)

#define IDENT_FREQ	24000000U	/* HOSC: no sample delay needed */
#define HOSC_HZ		24000000U
#define CCU_BASE	0x02001000U
#define CCU_PLL_PERI	0x0020

#define TUNE_DELAY_SETTLE_US	1000

/* the tuning stores its result in 32 byte records */
#define REC_MAGIC	"SPIFTUNE"
#define REC_SIZE	32U

struct spif_rec {
	uint8_t magic[8];
	uint8_t jedec[3];
	uint8_t mode;
	uint32_t frequency;	/* the frequency the sample point is good for */
	uint32_t requested;	/* the frequency asked for by the devicetree */
	uint8_t delay;
	uint8_t dtr;		/* the sample point is for DTR reads */
	uint8_t reserved[6];
	uint32_t crc;
};
BUILD_ASSERT(sizeof(struct spif_rec) == REC_SIZE);

struct spif_config {
	const struct pinctrl_dev_config *pcfg;
	const struct device *clock_dev;
	uintptr_t base;
	uintptr_t xip_base;
	size_t xip_size;
	uint32_t bus_clock_id;
	struct reset_dt_spec reset;
	uint32_t clock_reg;
	uint32_t frequency;
	uint32_t size;		/* bytes, 0: from the JEDEC ID */
	uint8_t jedec[3];
	bool has_jedec;
	bool quad;
	bool dtr;
	uint8_t dtr_opcode;
	uint8_t dtr_dummy;	/* 0: probe */
	bool addr_4byte;
	bool clear_bp;
	bool has_params;
	uint32_t params_offset;
	uint32_t params_size;
	uint32_t tune_offset;
	bool xip_at_init;
	uint32_t xip_offset;
	uint32_t xip_length;
};

struct spif_cmd {
	uint8_t opcode;
	uint8_t addr_bytes;	/* 0, 3 or 4 */
	uint32_t addr;
	uint8_t dummy;		/* dummy clock cycles */
	uint8_t addr_width;	/* 1, 2 or 4 wires */
	uint8_t data_width;
	bool mode;		/* a mode byte follows the address */
	uint8_t mode_val;
	uint8_t mode_width;
	bool write;		/* data goes to the flash */
	uint32_t len;		/* data bytes, in/from buf (default: the bounce buffer) */
	void *buf;		/* 64 byte aligned, a multiple of 64 bytes long when set */
};

struct spif_xip_regs {
	uint32_t phc, tcf, tcs, tnm;
};

struct spif_data {
	struct k_sem lock;
	uint32_t version;
	uint8_t jedec[3];
	uint32_t size;
	bool addr_4byte;
	bool quad;
	bool dtr_cap;		/* DTR reads work (probed) */
	bool dtr_on;		/* reads use DTR now */
	bool dtr_active;	/* the controller is in DTR mode right now */
	uint8_t dtr_dummy;
	uint8_t dtr_opcode;
	uint32_t frequency;
	bool tuned;
	uint8_t mode;
	uint8_t delay;
	bool sample_en;
	struct flash_pages_layout layout;

	bool xip;
	uint32_t xip_offset;
	uint32_t xip_length;
	struct spif_xip_regs xip_regs;
	unsigned int irq_key;
	bool xip_suspended;
	/* flash range written while the mapping was suspended */
	uint32_t dirty_lo;
	uint32_t dirty_hi;

	/* the DMA works on these: cache line aligned and a multiple of it */
	uint8_t desc[DESC_COUNT * DMA_DESC_BYTES] __aligned(64);
	uint8_t buf[BOUNCE_SIZE] __aligned(64);
	uint8_t ref[BOUNCE_SIZE] __aligned(64);
};

static inline uint32_t spif_rd(const struct spif_config *cfg, uint32_t reg)
{
	return sys_read32(cfg->base + reg);
}

static inline void spif_wr(const struct spif_config *cfg, uint32_t reg, uint32_t val)
{
	sys_write32(val, cfg->base + reg);
}

static inline void spif_rmw(const struct spif_config *cfg, uint32_t reg, uint32_t clr,
			    uint32_t set)
{
	spif_wr(cfg, reg, (spif_rd(cfg, reg) & ~clr) | set);
}

static int spif_wait_clear(const struct spif_config *cfg, uint32_t reg, uint32_t mask,
			   uint32_t timeout_us)
{
	for (uint32_t i = 0; i < 4096U; i++) {
		if ((spif_rd(cfg, reg) & mask) == 0U) {
			return 0;
		}
	}
	for (uint32_t t = 0; t < timeout_us; t += 5U) {
		if ((spif_rd(cfg, reg) & mask) == 0U) {
			return 0;
		}
		k_busy_wait(5);
	}

	return -ETIMEDOUT;
}

static int spif_wait_set(const struct spif_config *cfg, uint32_t reg, uint32_t mask,
			 uint32_t timeout_us)
{
	for (uint32_t i = 0; i < 4096U; i++) {
		if ((spif_rd(cfg, reg) & mask) != 0U) {
			return 0;
		}
	}
	for (uint32_t t = 0; t < timeout_us; t += 5U) {
		if ((spif_rd(cfg, reg) & mask) != 0U) {
			return 0;
		}
		k_busy_wait(5);
	}

	return -ETIMEDOUT;
}

/* ---- clock ------------------------------------------------------------ */

static uint32_t spif_pll_peri_1x(void)
{
	uint32_t pll = sys_read32(CCU_BASE + CCU_PLL_PERI);
	uint32_t n = FIELD_GET(GENMASK(15, 8), pll) + 1U;
	uint32_t p0 = FIELD_GET(GENMASK(18, 16), pll) + 1U;
	uint32_t m = (pll & BIT(1)) ? 2U : 1U;

	return HOSC_HZ / m / p0 * n / 2U;
}

/* module clock: 24 MHz or PLL_PERI_1X, M (1..16) and P (1,2,4,8) dividers */
static uint32_t spif_set_clock(const struct device *dev, uint32_t hz)
{
	const struct spif_config *cfg = dev->config;
	uint32_t src_hz = hz > HOSC_HZ ? spif_pll_peri_1x() : HOSC_HZ;
	uint32_t mux = hz > HOSC_HZ ? 1U : 0U;
	uint32_t best_m = 0, best_p = 0, best = 0;

	for (uint32_t p = 0; p < 4U; p++) {
		for (uint32_t m = 0; m < 16U; m++) {
			uint32_t rate = src_hz / (m + 1U) / BIT(p);

			if (rate <= hz && rate > best) {
				best = rate;
				best_m = m;
				best_p = p;
			}
		}
	}
	if (best == 0U) {
		best_m = 15U;
		best_p = 3U;
		best = src_hz / 16U / 8U;
	}

	sys_write32(BIT(31) | (mux << 24) | (best_p << 8) | best_m, CCU_BASE + cfg->clock_reg);

	return best;
}

/* ---- controller ------------------------------------------------------- */

static void spif_fifo_reset(const struct spif_config *cfg)
{
	spif_rmw(cfg, SPIF_GCA, 0, GCA_FIFO_RST);
	(void)spif_wait_clear(cfg, SPIF_GCA, GCA_FIFO_RST, 1000);
}

static void spif_soft_reset(const struct spif_config *cfg)
{
	spif_rmw(cfg, SPIF_GCA, 0, GCA_DMA_END);
	(void)spif_wait_clear(cfg, SPIF_GCA, GCA_DMA_END, 1000);
	spif_rmw(cfg, SPIF_GCA, 0, GCA_SOFT_RST);
	(void)spif_wait_clear(cfg, SPIF_GCA, GCA_SOFT_RST, 1000);
}

static void spif_set_tc(const struct device *dev, bool enable, uint8_t mode, uint8_t delay)
{
	const struct spif_config *cfg = dev->config;
	struct spif_data *d = dev->data;
	uint32_t tc = spif_rd(cfg, SPIF_TC);

	tc &= ~(TC_SAMPLE_EN | TC_DELAY_SW_EN | TC_MODE_MASK | TC_DELAY_MASK);
	if (enable) {
		tc |= TC_SAMPLE_EN | TC_DELAY_SW_EN | FIELD_PREP(TC_MODE_MASK, mode) |
		      FIELD_PREP(TC_DELAY_MASK, delay);
	}
	spif_wr(cfg, SPIF_TC, tc);
	d->sample_en = enable;
	/* the delay line needs a moment to settle */
	k_busy_wait(TUNE_DELAY_SETTLE_US);
}

static void spif_hw_setup(const struct spif_config *cfg)
{
	spif_soft_reset(cfg);
	spif_fifo_reset(cfg);
	spif_rmw(cfg, SPIF_GC, GC_NMODE_EN | GC_PMODE_EN, 0);
	/* MSB first, no write protect/hold pins driven, chip select 0 active low, mode 0 */
	spif_rmw(cfg, SPIF_GC, GC_RX_FBS | GC_TX_FBS | GC_WP_EN | GC_HOLD_EN | GC_DTR_EN |
		 GC_SS_MASK | GC_CPHA | GC_CPOL, GC_CS_POL);
	spif_rmw(cfg, SPIF_TC, TC_SCKOUT_SEL, 0);
	spif_wr(cfg, SPIF_CSD, CSD_DEFAULT);
	spif_wr(cfg, SPIF_INT_EN, 0);
	spif_wr(cfg, SPIF_INT_STA, 0xffffffffU);
}

static inline uint32_t width_code(uint8_t width)
{
	return width == 4U ? 2U : (width == 2U ? 1U : 0U);
}

/* the four words that describe a command */
static void spif_build(const struct spif_data *d, const struct spif_cmd *c, uint32_t *phc,
		       uint32_t *tcf, uint32_t *tcs, uint32_t *tnm)
{
	*phc = PHC_CMD;
	*tcf = 0;
	*tcs = ((uint32_t)c->opcode << TCS_OPCODE_SHIFT);
	*tnm = TNM_NORMAL_EN;

	if (c->addr_bytes != 0U) {
		uint32_t size;

		*phc |= PHC_ADDR;
		*tcf = c->addr;
		*tcs |= width_code(c->addr_width) << TCS_ADDR_SHIFT;
		if (d->version >= SPIF_VER_V2) {
			size = c->addr_bytes == 4U ? 3U : 2U;
		} else {
			size = c->addr_bytes == 4U ? 1U : 0U;
		}
		*tnm |= size << 24;
	}
	if (c->mode) {
		*phc |= PHC_MODE;
		*tcs |= ((uint32_t)c->mode_val << TCS_MODE_OPCODE_SHIFT) |
			(width_code(c->mode_width) << TCS_MODE_SHIFT);
	}
	if (c->dummy != 0U) {
		*phc |= PHC_DUMMY;
		*tnm |= (uint32_t)c->dummy << TNM_DUMMY_SHIFT;
	}
	if (c->len != 0U) {
		*phc |= c->write ? PHC_TX : PHC_RX;
		*tcs |= width_code(c->data_width) << TCS_DATA_SHIFT;
		*tnm |= c->len >= 65536U ? TNM_LEN_64K : c->len;
	}
}

static int spif_recover(const struct device *dev)
{
	const struct spif_config *cfg = dev->config;
	uint32_t tc = spif_rd(cfg, SPIF_TC);

	struct spif_data *d = dev->data;

	spif_hw_setup(cfg);
	spif_wr(cfg, SPIF_TC, tc);
	d->dtr_active = false;
	spif_set_clock(dev, d->frequency);

	return -ETIMEDOUT;
}

/* Run one command. Data is read to or written from d->buf. */
static int spif_xfer(const struct device *dev, const struct spif_cmd *c)
{
	const struct spif_config *cfg = dev->config;
	struct spif_data *d = dev->data;
	uint32_t phc, tcf, tcs, tnm;

	spif_build(d, c, &phc, &tcf, &tcs, &tnm);
	spif_fifo_reset(cfg);

	if (c->len == 0U) {
		spif_rmw(cfg, SPIF_GC, GC_DMA_MODE, 0);
		spif_wr(cfg, SPIF_PHC, phc);
		spif_wr(cfg, SPIF_TCF, tcf);
		spif_wr(cfg, SPIF_TCS, tcs);
		spif_wr(cfg, SPIF_TNM, tnm);
		spif_rmw(cfg, SPIF_GC, 0, GC_NMODE_EN);
		if (spif_wait_clear(cfg, SPIF_GC, GC_NMODE_EN, 100000) != 0) {
			LOG_ERR("command %02x timed out", c->opcode);
			return spif_recover(dev);
		}

		return 0;
	}

	uint32_t *desc = (uint32_t *)d->desc;
	uint8_t *buf = c->buf != NULL ? c->buf : d->buf;
	uint32_t blen = ROUND_UP(c->len, 64U);
	uint32_t ndesc = DIV_ROUND_UP(c->len, DESC_MAX_LEN);

	if (ndesc > DESC_COUNT) {
		return -EINVAL;
	}
	/* a chain of descriptors, each moves up to 64 KiB with the same command */
	for (uint32_t i = 0; i < ndesc; i++) {
		uint32_t *w = &desc[i * 8U];
		uint32_t n = MIN(c->len - i * DESC_MAX_LEN, DESC_MAX_LEN);
		bool last = i == ndesc - 1U;

		w[0] = DESC_BURST_INCR16 | (last ? DESC_LAST : 0U) | (c->write ? 0U : DESC_READ);
		w[1] = DESC_BLOCK_64B | n;
		w[2] = (uint32_t)(uintptr_t)(buf + i * DESC_MAX_LEN) >> 2;
		w[3] = last ? 0U : (uint32_t)(uintptr_t)&desc[(i + 1U) * 8U] >> 2;
		w[4] = phc;
		w[5] = tcf + i * DESC_MAX_LEN;
		w[6] = tcs;
		w[7] = (tnm & ~(TNM_LEN_64K | GENMASK(15, 0))) |
		       (n == DESC_MAX_LEN ? TNM_LEN_64K : n);
	}

	sys_cache_data_flush_range(d->desc, ndesc * DMA_DESC_BYTES);
	if (c->write) {
		sys_cache_data_flush_range(buf, blen);
	} else {
		sys_cache_data_flush_and_invd_range(buf, blen);
	}

	spif_rmw(cfg, SPIF_GC, 0, GC_DMA_MODE);
	spif_wr(cfg, SPIF_INT_STA, INT_DMA_DONE | INT_ERR);
	spif_wr(cfg, SPIF_DSC, (uint32_t)(uintptr_t)d->desc >> 2);
	spif_rmw(cfg, SPIF_DMA_CTL, DMA_CTL_DESC_LEN_MASK, DMA_DESC_BYTES << 4);
	spif_rmw(cfg, SPIF_DMA_CTL, 0, DMA_CTL_START);

	if (spif_wait_set(cfg, SPIF_INT_STA, INT_DMA_DONE | INT_ERR,
			  200000U + ndesc * 20000U) != 0) {
		LOG_ERR("DMA of command %02x timed out", c->opcode);
		return spif_recover(dev);
	}
	if ((spif_rd(cfg, SPIF_INT_STA) & INT_ERR) != 0U) {
		LOG_ERR("DMA error %08x", spif_rd(cfg, SPIF_INT_STA));
		spif_wr(cfg, SPIF_INT_STA, INT_DMA_DONE | INT_ERR);
		spif_recover(dev);

		return -EIO;
	}
	spif_wr(cfg, SPIF_INT_STA, INT_DMA_DONE);
	if (!c->write) {
		sys_cache_data_invd_range(buf, blen);
	}

	return 0;
}

/*
 * Double data rate: address and data change on both clock edges, the module
 * clock runs at twice the SCK. Only the read command uses it.
 */
static void spif_dtr_set(const struct device *dev, bool on)
{
	const struct spif_config *cfg = dev->config;
	struct spif_data *d = dev->data;

	if (on == d->dtr_active) {
		return;
	}
	spif_rmw(cfg, SPIF_TC, TC_SCKOUT_SEL, on ? TC_SCKOUT_SEL : 0);
	spif_rmw(cfg, SPIF_GC, GC_DTR_EN, on ? GC_DTR_EN : 0);
	spif_set_clock(dev, on ? 2U * d->frequency : d->frequency);
	spif_soft_reset(cfg);
	d->dtr_active = on;
}

/* the read command in use: DTR 1-4-4 with a mode byte, 1-1-4 or one wire */
static void spif_read_cmd(const struct spif_data *d, uint32_t off, uint32_t len,
			  struct spif_cmd *c)
{
	memset(c, 0, sizeof(*c));
	c->addr_bytes = d->addr_4byte ? 4U : 3U;
	c->addr = off;
	c->len = len;
	c->addr_width = 1;
	if (d->dtr_on) {
		c->opcode = d->dtr_opcode;
		c->addr_width = 4;
		c->data_width = 4;
		c->mode = true;
		c->mode_val = DTR_MODE_BYTE;
		c->mode_width = 4;
		c->dummy = d->dtr_dummy;
		return;
	}
	c->dummy = 8;
	c->data_width = d->quad ? 4U : 1U;
	c->opcode = d->quad ? (d->addr_4byte ? NOR_FAST_READ_QUAD_4B : NOR_FAST_READ_QUAD)
			    : (d->addr_4byte ? NOR_FAST_READ_4B : NOR_FAST_READ);
}

/* ---- XIP mapping ------------------------------------------------------ */

static void spif_window_regs(const struct device *dev, struct spif_xip_regs *r)
{
	struct spif_data *d = dev->data;
	struct spif_cmd c;

	spif_read_cmd(d, d->xip_offset, 1, &c);
	spif_build(d, &c, &r->phc, &r->tcf, &r->tcs, &r->tnm);
	/* a window read is described without the descriptor enable bit */
	r->tnm &= ~(TNM_NORMAL_EN | GENMASK(15, 0));
}

static void spif_window_on(const struct device *dev, const struct spif_xip_regs *r)
{
	const struct spif_config *cfg = dev->config;
	struct spif_data *d = dev->data;

	spif_dtr_set(dev, d->dtr_on);
	spif_rmw(cfg, SPIF_GC, GC_NMODE_EN | GC_PMODE_EN | GC_DMA_MODE, 0);
	spif_wr(cfg, SPIF_PSA, cfg->xip_base);
	spif_wr(cfg, SPIF_PEA, cfg->xip_base + d->xip_length);
	spif_rmw(cfg, SPIF_GC, 0, GC_ADDR_MAP);
	spif_wr(cfg, SPIF_PMA, d->xip_offset);
	spif_wr(cfg, SPIF_PHC, r->phc);
	spif_wr(cfg, SPIF_TCF, r->tcf);
	spif_wr(cfg, SPIF_TCS, r->tcs);
	spif_wr(cfg, SPIF_TNM, r->tnm);
	spif_rmw(cfg, SPIF_GC, 0, GC_PMODE_EN);
}

static void spif_window_off(const struct device *dev)
{
	const struct spif_config *cfg = dev->config;

	spif_dtr_set(dev, false);
	spif_soft_reset(cfg);
	spif_fifo_reset(cfg);
	spif_rmw(cfg, SPIF_GC, GC_NMODE_EN | GC_PMODE_EN | GC_ADDR_MAP, 0);
	spif_wr(cfg, SPIF_PMA, 0);
}

/*
 * Bracket every access to the flash: with a mapping active it is taken down
 * for the duration of the command, with interrupts masked so that nothing
 * can run from (or read) the window meanwhile.
 */
static void spif_begin(const struct device *dev)
{
	struct spif_data *d = dev->data;

	if (!d->xip) {
		return;
	}
	d->irq_key = irq_lock();
	d->xip_suspended = true;
	spif_window_off(dev);
}

static void spif_end(const struct device *dev)
{
	const struct spif_config *cfg = dev->config;
	struct spif_data *d = dev->data;

	if (!d->xip) {
		return;
	}
	spif_soft_reset(cfg);
	spif_window_on(dev, &d->xip_regs);
	if (d->dirty_hi > d->dirty_lo) {
		uint32_t lo = MAX(d->dirty_lo, d->xip_offset);
		uint32_t hi = MIN(d->dirty_hi, d->xip_offset + d->xip_length);

		if (hi > lo) {
			uintptr_t start = cfg->xip_base + (lo - d->xip_offset);

			sys_cache_data_invd_range((void *)start, hi - lo);
			sys_cache_instr_invd_range((void *)start, hi - lo);
		}
	}
	d->dirty_lo = UINT32_MAX;
	d->dirty_hi = 0;
	d->xip_suspended = false;
	irq_unlock(d->irq_key);
}

static void spif_mark_dirty(struct spif_data *d, uint32_t off, uint32_t len)
{
	d->dirty_lo = MIN(d->dirty_lo, off);
	d->dirty_hi = MAX(d->dirty_hi, off + len);
}

/* ---- NOR commands ----------------------------------------------------- */

static int nor_simple(const struct device *dev, uint8_t opcode, uint32_t rx_len,
		      const uint8_t *tx, uint32_t tx_len)
{
	struct spif_data *d = dev->data;
	struct spif_cmd c = { .opcode = opcode, .data_width = 1 };
	int ret;

	if (rx_len != 0U) {
		c.len = rx_len;
	} else if (tx_len != 0U) {
		c.len = tx_len;
		c.write = true;
		memcpy(d->buf, tx, tx_len);
	}
	ret = spif_xfer(dev, &c);

	return ret;
}

static int nor_read_status(const struct device *dev, uint8_t opcode, uint8_t *sr)
{
	struct spif_data *d = dev->data;
	int ret = nor_simple(dev, opcode, 1, NULL, 0);

	if (ret == 0) {
		*sr = d->buf[0];
	}

	return ret;
}

static int nor_write_enable(const struct device *dev)
{
	return nor_simple(dev, NOR_WREN, 0, NULL, 0);
}

/* poll the work in progress bit; the caller holds the bracket */
static int nor_wait_ready(const struct device *dev, uint32_t timeout_ms)
{
	struct spif_data *d = dev->data;
	int64_t end = k_uptime_get() + timeout_ms;
	uint8_t sr;
	int ret;

	do {
		ret = nor_read_status(dev, NOR_RDSR, &sr);
		if (ret != 0) {
			return ret;
		}
		if ((sr & SR_WIP) == 0U) {
			return 0;
		}
		if (d->xip_suspended) {
			k_busy_wait(100);
		} else {
			k_busy_wait(20);
		}
	} while (k_uptime_get() < end);

	return -ETIMEDOUT;
}

static int nor_enable_quad(const struct device *dev)
{
	struct spif_data *d = dev->data;
	uint8_t mfr = d->jedec[0];
	uint8_t sr, sr2;
	int ret;

	if (mfr == 0xc2 || mfr == 0x9d) {
		/* Macronix, ISSI: quad enable is bit 6 of status register 1 */
		ret = nor_read_status(dev, NOR_RDSR, &sr);
		if (ret != 0) {
			return ret;
		}
		if ((sr & SR_QE_SR1) == 0U) {
			sr |= SR_QE_SR1;
			if (nor_write_enable(dev) != 0 ||
			    nor_simple(dev, NOR_WRSR, 0, &sr, 1) != 0 ||
			    nor_wait_ready(dev, 100) != 0) {
				return -EIO;
			}
			ret = nor_read_status(dev, NOR_RDSR, &sr);
			if (ret != 0 || (sr & SR_QE_SR1) == 0U) {
				return -EIO;
			}
		}

		return 0;
	}

	/* the others keep it in bit 1 of status register 2 */
	ret = nor_read_status(dev, NOR_RDSR2, &sr2);
	if (ret != 0) {
		return ret;
	}
	if ((sr2 & SR2_QE) != 0U) {
		return 0;
	}
	sr2 |= SR2_QE;
	if (nor_write_enable(dev) != 0 || nor_simple(dev, NOR_WRSR2, 0, &sr2, 1) != 0 ||
	    nor_wait_ready(dev, 100) != 0) {
		return -EIO;
	}
	ret = nor_read_status(dev, NOR_RDSR2, &sr2);
	if (ret == 0 && (sr2 & SR2_QE) != 0U) {
		return 0;
	}

	/* parts that take both status registers with one write command */
	uint8_t both[2];

	if (nor_read_status(dev, NOR_RDSR, &both[0]) != 0) {
		return -EIO;
	}
	both[1] = sr2 | SR2_QE;
	if (nor_write_enable(dev) != 0 || nor_simple(dev, NOR_WRSR, 0, both, 2) != 0 ||
	    nor_wait_ready(dev, 100) != 0) {
		return -EIO;
	}
	ret = nor_read_status(dev, NOR_RDSR2, &sr2);

	return (ret == 0 && (sr2 & SR2_QE) != 0U) ? 0 : -EIO;
}

/*
 * Block protect bits: BP0..BP2, TB and SEC in bits 2..6 of status register 1
 * (BP0..BP3 in 2..5 and the quad enable in bit 6 for Macronix and ISSI), and
 * CMP in bit 6 of status register 2. Status register 1 and 2 are written
 * together so that no part resets the quad enable bit.
 */
static int nor_clear_protect(const struct device *dev, uint8_t *sr1_out)
{
	struct spif_data *d = dev->data;
	bool sr1_qe = d->jedec[0] == 0xc2 || d->jedec[0] == 0x9d;
	uint8_t mask = sr1_qe ? 0x3c : 0x7c;
	uint8_t sr1, sr2 = 0, wr[2];
	int ret = nor_read_status(dev, NOR_RDSR, &sr1);

	if (ret == 0 && !sr1_qe) {
		ret = nor_read_status(dev, NOR_RDSR2, &sr2);
	}
	if (ret != 0) {
		return ret;
	}
	*sr1_out = sr1;
	if ((sr1 & mask) == 0U && (sr2 & SR2_CMP) == 0U) {
		return 0;
	}
	wr[0] = sr1 & ~mask;
	wr[1] = sr2 & ~SR2_CMP;
	ret = nor_write_enable(dev);
	if (ret == 0) {
		ret = nor_simple(dev, NOR_WRSR, 0, wr, sr1_qe ? 1 : 2);
	}
	if (ret == 0) {
		ret = nor_wait_ready(dev, 500);
	}
	if (ret == 0) {
		ret = nor_read_status(dev, NOR_RDSR, &sr1);
	}
	if (ret == 0 && (sr1 & mask) != 0U) {
		return -EACCES;
	}
	*sr1_out = sr1;

	return ret;
}

/* read into buf (NULL: the bounce buffer, up to BOUNCE_SIZE bytes) */
static int nor_read_to(const struct device *dev, uint32_t off, void *buf, uint32_t len)
{
	struct spif_data *d = dev->data;
	struct spif_cmd c;
	int ret;

	spif_read_cmd(d, off, len, &c);
	c.buf = buf;
	spif_begin(dev);
	spif_dtr_set(dev, d->dtr_on);
	ret = spif_xfer(dev, &c);
	spif_dtr_set(dev, false);
	spif_end(dev);

	return ret;
}

static int nor_read_chunk(const struct device *dev, uint32_t off, uint32_t len)
{
	return nor_read_to(dev, off, NULL, len);
}

static int nor_program_page(const struct device *dev, uint32_t off, uint32_t len)
{
	struct spif_data *d = dev->data;
	struct spif_cmd c = {
		.opcode = d->quad ? (d->addr_4byte ? NOR_PP_QUAD_4B : NOR_PP_QUAD)
				  : (d->addr_4byte ? NOR_PP_4B : NOR_PP),
		.addr_bytes = d->addr_4byte ? 4U : 3U,
		.addr = off,
		.addr_width = 1,
		.data_width = d->quad ? 4U : 1U,
		.write = true,
		.len = len,
	};
	int ret;

	spif_begin(dev);
	ret = nor_write_enable(dev);
	if (ret == 0) {
		ret = spif_xfer(dev, &c);
	}
	if (ret == 0) {
		ret = nor_wait_ready(dev, 50);
		spif_mark_dirty(d, off, len);
	}
	spif_end(dev);

	return ret;
}

static int nor_erase_block(const struct device *dev, uint32_t off, bool big)
{
	struct spif_data *d = dev->data;
	struct spif_cmd c = {
		.opcode = big ? (d->addr_4byte ? NOR_BE64_4B : NOR_BE64)
			      : (d->addr_4byte ? NOR_SE_4B : NOR_SE),
		.addr_bytes = d->addr_4byte ? 4U : 3U,
		.addr = off,
		.addr_width = 1,
	};
	int ret;

	spif_begin(dev);
	ret = nor_write_enable(dev);
	if (ret == 0) {
		ret = spif_xfer(dev, &c);
	}
	if (ret == 0) {
		ret = nor_wait_ready(dev, big ? 3000 : 1000);
		spif_mark_dirty(d, off, big ? BLOCK_SIZE : SECTOR_SIZE);
	}
	spif_end(dev);

	return ret;
}

/* ---- flash API -------------------------------------------------------- */

static int spif_check_range(const struct spif_data *d, off_t off, size_t len)
{
	if (off < 0 || (uint64_t)off + len > d->size) {
		return -EINVAL;
	}

	return 0;
}

static int spif_flash_read(const struct device *dev, off_t off, void *dst, size_t len)
{
	struct spif_data *d = dev->data;
	uint8_t *out = dst;
	int ret = spif_check_range(d, off, len);

	if (ret != 0) {
		return ret;
	}
	k_sem_take(&d->lock, K_FOREVER);
	while (len != 0U && ret == 0) {
		uint32_t n;

		if (((uintptr_t)out % 64U) == 0U && len >= 64U) {
			/* straight into the caller's buffer, a whole number of cache lines */
			n = MIN(ROUND_DOWN(len, 64U), DIRECT_MAX);
			ret = nor_read_to(dev, off, out, n);
		} else {
			n = MIN(len, BOUNCE_SIZE);
			ret = nor_read_chunk(dev, off, n);
			if (ret == 0) {
				memcpy(out, d->buf, n);
			}
		}
		if (ret == 0) {
			off += n;
			out += n;
			len -= n;
		}
	}
	k_sem_give(&d->lock);

	return ret;
}

static int spif_flash_write(const struct device *dev, off_t off, const void *src, size_t len)
{
	struct spif_data *d = dev->data;
	const uint8_t *in = src;
	int ret = spif_check_range(d, off, len);

	if (ret != 0) {
		return ret;
	}
	k_sem_take(&d->lock, K_FOREVER);
	while (len != 0U && ret == 0) {
		uint32_t n = MIN(len, PAGE_SIZE_NOR - ((uint32_t)off % PAGE_SIZE_NOR));

		memcpy(d->buf, in, n);
		ret = nor_program_page(dev, off, n);
		off += n;
		in += n;
		len -= n;
	}
	k_sem_give(&d->lock);

	return ret;
}

static int spif_flash_erase(const struct device *dev, off_t off, size_t len)
{
	struct spif_data *d = dev->data;
	int ret = spif_check_range(d, off, len);

	if (ret != 0) {
		return ret;
	}
	if ((off % SECTOR_SIZE) != 0 || (len % SECTOR_SIZE) != 0U) {
		return -EINVAL;
	}
	k_sem_take(&d->lock, K_FOREVER);
	while (len != 0U && ret == 0) {
		bool big = (off % BLOCK_SIZE) == 0 && len >= BLOCK_SIZE;
		uint32_t n = big ? BLOCK_SIZE : SECTOR_SIZE;

		ret = nor_erase_block(dev, off, big);
		off += n;
		len -= n;
	}
	k_sem_give(&d->lock);

	return ret;
}

static const struct flash_parameters spif_flash_parameters = {
	.write_block_size = 1,
	.erase_value = 0xff,
};

static const struct flash_parameters *spif_flash_get_parameters(const struct device *dev)
{
	ARG_UNUSED(dev);

	return &spif_flash_parameters;
}

static int spif_flash_get_size(const struct device *dev, uint64_t *size)
{
	const struct spif_data *d = dev->data;

	*size = d->size;

	return 0;
}

#if defined(CONFIG_FLASH_PAGE_LAYOUT)
static void spif_flash_page_layout(const struct device *dev,
				   const struct flash_pages_layout **layout, size_t *count)
{
	const struct spif_data *d = dev->data;

	*layout = &d->layout;
	*count = 1;
}
#endif

/* ---- tuning and stored parameters ------------------------------------ */

static void spif_apply_op_point(const struct device *dev, uint32_t hz, bool tuned, uint8_t mode,
				uint8_t delay)
{
	struct spif_data *d = dev->data;

	d->frequency = spif_set_clock(dev, hz);
	spif_set_tc(dev, tuned, mode, delay);
	d->tuned = tuned;
	d->dtr_on = tuned && d->dtr_cap;
	d->mode = mode;
	d->delay = delay;
}

/* the operating point is saved around work that has to run at the safe one */
struct spif_point {
	uint32_t hz;
	bool tuned;
	uint8_t mode, delay;
};

static void spif_point_save(const struct device *dev, struct spif_point *p)
{
	const struct spif_data *d = dev->data;

	p->hz = d->frequency;
	p->tuned = d->tuned;
	p->mode = d->mode;
	p->delay = d->delay;
}

static void spif_point_restore(const struct device *dev, const struct spif_point *p)
{
	spif_apply_op_point(dev, p->hz, p->tuned, p->mode, p->delay);
}

int sunxi_spif_set_sample(const struct device *dev, uint8_t mode, uint8_t delay)
{
	const struct spif_config *cfg = dev->config;
	struct spif_data *d = dev->data;

	if (mode >= SUNXI_SPIF_TUNE_MODES || delay >= SUNXI_SPIF_TUNE_DELAYS) {
		return -EINVAL;
	}
	k_sem_take(&d->lock, K_FOREVER);
	if (d->xip) {
		k_sem_give(&d->lock);
		return -EBUSY;
	}
	spif_apply_op_point(dev, cfg->frequency, true, mode, delay);
	k_sem_give(&d->lock);

	return 0;
}

static bool spif_data_is_usable(const uint8_t *p, size_t n)
{
	uint32_t seen[8] = { 0 };
	uint32_t distinct = 0;

	for (size_t i = 0; i < n; i++) {
		uint32_t *w = &seen[p[i] / 32U];
		uint32_t bit = BIT(p[i] % 32U);

		if ((*w & bit) == 0U) {
			*w |= bit;
			distinct++;
		}
	}

	return distinct >= 32U;
}

/* find out whether DTR reads work, and with how many dummy cycles */
static void spif_dtr_probe(const struct device *dev)
{
	const struct spif_config *cfg = dev->config;
	struct spif_data *d = dev->data;
	uint32_t off = cfg->tune_offset;
	uint8_t first = cfg->dtr_dummy != 0U ? cfg->dtr_dummy : 1U;
	uint8_t last = cfg->dtr_dummy != 0U ? cfg->dtr_dummy : DTR_DUMMY_MAX;

	d->dtr_cap = false;
	d->dtr_on = false;
	if (!cfg->dtr || !d->quad || d->addr_4byte || off + BOUNCE_SIZE > d->size) {
		return;
	}
	if (nor_read_chunk(dev, off, BOUNCE_SIZE) != 0) {
		return;
	}
	memcpy(d->ref, d->buf, BOUNCE_SIZE);
	if (!spif_data_is_usable(d->ref, BOUNCE_SIZE)) {
		LOG_WRN("tune partition holds no usable data, DTR not probed");
		return;
	}
	d->dtr_opcode = cfg->dtr_opcode;
	d->dtr_on = true;
	for (uint8_t n = first; n <= last; n++) {
		d->dtr_dummy = n;
		memset(d->buf, 0, BOUNCE_SIZE);
		if (nor_read_chunk(dev, off, BOUNCE_SIZE) == 0 &&
		    memcmp(d->buf, d->ref, BOUNCE_SIZE) == 0) {
			d->dtr_cap = true;
			LOG_INF("DTR read %02x works with %u dummy cycles", d->dtr_opcode, n);
			break;
		}
	}
	d->dtr_on = false;
	if (!d->dtr_cap) {
		LOG_WRN("DTR read %02x does not work, using SDR", cfg->dtr_opcode);
	}
}

/* the one wire command the points are checked with, besides the data read */
static bool spif_id_ok(const struct device *dev)
{
	struct spif_data *d = dev->data;

	return nor_simple(dev, NOR_RDID, 3, NULL, 0) == 0 && memcmp(d->buf, d->jedec, 3) == 0;
}

/*
 * Try every sample mode and delay at the current frequency. A point is good
 * when the JEDEC ID (one wire) and the reference data (the read mode in use)
 * both come back right, twice. Returns the width of the widest window.
 */
static uint32_t spif_scan(const struct device *dev, uint32_t off, struct sunxi_spif_tune_result *r,
			  uint8_t *best_mode, uint8_t *best_start)
{
	struct spif_data *d = dev->data;
	uint32_t best_len = 0;

	memset(r->ok, 0, sizeof(r->ok));
	memset(r->ok_one_wire, 0, sizeof(r->ok_one_wire));
	for (uint8_t mode = 0; mode < SUNXI_SPIF_TUNE_MODES; mode++) {
		uint32_t run = 0;

		for (uint8_t delay = 0; delay < SUNXI_SPIF_TUNE_DELAYS; delay++) {
			bool one = true, data = true;

			spif_set_tc(dev, true, mode, delay);
			for (int pass = 0; pass < 2; pass++) {
				spif_begin(dev);
				one = one && spif_id_ok(dev);
				spif_end(dev);
				memset(d->buf, 0, BOUNCE_SIZE);
				data = data && nor_read_chunk(dev, off, BOUNCE_SIZE) == 0 &&
				       memcmp(d->buf, d->ref, BOUNCE_SIZE) == 0;
			}
			if (one) {
				r->ok_one_wire[mode] |= BIT64(delay);
			}
			if (one && data) {
				r->ok[mode] |= BIT64(delay);
				run++;
				if (run > best_len) {
					best_len = run;
					*best_start = delay + 1U - run;
					*best_mode = mode;
				}
			} else {
				run = 0;
			}
		}
	}

	return best_len;
}

/* frequencies tried in turn when no window is found at the requested one */
static const uint32_t tune_fallback_hz[] = { 75000000, 60000000, 50000000, 40000000, 30000000 };

int sunxi_spif_tune(const struct device *dev, struct sunxi_spif_tune_result *res)
{
	const struct spif_config *cfg = dev->config;
	struct spif_data *d = dev->data;
	struct sunxi_spif_tune_result r = { 0 };
	uint32_t off = cfg->tune_offset;
	uint8_t best_mode = 0, best_start = 0;
	uint32_t best_len;
	int ret;

	if (off + BOUNCE_SIZE > d->size) {
		return -EINVAL;
	}
	k_sem_take(&d->lock, K_FOREVER);
	if (d->xip) {
		k_sem_give(&d->lock);
		return -EBUSY;
	}

	/* the reference is read at a frequency that needs no tuning */
	spif_apply_op_point(dev, IDENT_FREQ, false, 0, 0);
	ret = nor_read_chunk(dev, off, BOUNCE_SIZE);
	if (ret != 0) {
		goto out;
	}
	memcpy(d->ref, d->buf, BOUNCE_SIZE);
	if (!spif_data_is_usable(d->ref, BOUNCE_SIZE)) {
		ret = -ENODATA;
		goto out;
	}

	ret = -EIO;
	for (int attempt = 0; attempt < 2 && ret != 0; attempt++) {
		if (attempt == 1) {
			if (!d->dtr_cap) {
				break;
			}
			LOG_WRN("no sample point for DTR reads, using SDR");
			d->dtr_cap = false;
		}
		for (int i = -1; i < (int)ARRAY_SIZE(tune_fallback_hz); i++) {
			uint32_t hz = i < 0 ? cfg->frequency : tune_fallback_hz[i];

			if (hz > cfg->frequency || hz <= IDENT_FREQ) {
				continue;
			}
			d->frequency = spif_set_clock(dev, hz);
			d->dtr_on = d->dtr_cap;
			r.frequency = d->frequency;
			best_len = spif_scan(dev, off, &r, &best_mode, &best_start);
			if (best_len >= CONFIG_FLASH_SUNXI_SPIF_TUNE_MIN_WINDOW) {
				r.mode = best_mode;
				r.window_start = best_start;
				r.window_len = best_len;
				r.delay = best_start + best_len / 2U;
				spif_apply_op_point(dev, hz, true, r.mode, r.delay);
				ret = 0;
				break;
			}
		}
	}
	r.dtr = d->dtr_cap;
	if (ret != 0) {
		spif_apply_op_point(dev, IDENT_FREQ, false, 0, 0);
	}
out:
	k_sem_give(&d->lock);
	if (res != NULL) {
		*res = r;
	}

	return ret;
}

static void spif_rec_make(const struct device *dev, struct spif_rec *rec)
{
	const struct spif_config *cfg = dev->config;
	const struct spif_data *d = dev->data;

	memset(rec, 0, sizeof(*rec));
	memcpy(rec->magic, REC_MAGIC, sizeof(rec->magic));
	memcpy(rec->jedec, d->jedec, 3);
	rec->mode = d->mode;
	rec->frequency = d->frequency;
	rec->requested = cfg->frequency;
	rec->delay = d->delay;
	rec->dtr = d->dtr_cap;
	rec->crc = crc32_ieee((const uint8_t *)rec, offsetof(struct spif_rec, crc));
}

static bool spif_rec_valid(const struct device *dev, const struct spif_rec *rec)
{
	const struct spif_config *cfg = dev->config;
	const struct spif_data *d = dev->data;

	return memcmp(rec->magic, REC_MAGIC, sizeof(rec->magic)) == 0 &&
	       rec->crc == crc32_ieee((const uint8_t *)rec, offsetof(struct spif_rec, crc)) &&
	       memcmp(rec->jedec, d->jedec, 3) == 0 && rec->requested == cfg->frequency &&
	       rec->frequency > IDENT_FREQ && rec->frequency <= cfg->frequency &&
	       rec->mode < SUNXI_SPIF_TUNE_MODES && rec->delay < SUNXI_SPIF_TUNE_DELAYS &&
	       rec->dtr == d->dtr_cap;
}

static bool spif_rec_blank(const struct spif_rec *rec)
{
	const uint8_t *p = (const uint8_t *)rec;

	for (size_t i = 0; i < sizeof(*rec); i++) {
		if (p[i] != 0xff) {
			return false;
		}
	}

	return true;
}

/*
 * Read the params sector at the safe operating point (the tuned one might be
 * the very thing that is wrong) and find the last valid record and the first
 * blank slot; -1 when there is none. The record of the last valid slot is
 * left in d->buf.
 */
static int spif_params_scan(const struct device *dev, int *last_valid, int *first_blank)
{
	const struct spif_config *cfg = dev->config;
	struct spif_data *d = dev->data;
	int ret = nor_read_chunk(dev, cfg->params_offset, SECTOR_SIZE);

	if (ret != 0) {
		return ret;
	}
	*last_valid = -1;
	*first_blank = -1;
	for (int i = 0; i < (int)(SECTOR_SIZE / REC_SIZE); i++) {
		const struct spif_rec *rec = (const struct spif_rec *)&d->buf[i * REC_SIZE];

		if (spif_rec_blank(rec)) {
			*first_blank = i;
			break;
		}
		if (spif_rec_valid(dev, rec)) {
			*last_valid = i;
		}
	}

	return 0;
}

int sunxi_spif_params_load(const struct device *dev)
{
	const struct spif_config *cfg = dev->config;
	struct spif_data *d = dev->data;
	struct spif_point saved;
	struct spif_rec rec;
	int last, blank, ret;

	if (!cfg->has_params) {
		return -ENOENT;
	}
	k_sem_take(&d->lock, K_FOREVER);
	if (d->xip) {
		k_sem_give(&d->lock);
		return -EBUSY;
	}
	spif_point_save(dev, &saved);
	spif_apply_op_point(dev, IDENT_FREQ, false, 0, 0);
	ret = spif_params_scan(dev, &last, &blank);
	if (ret == 0 && last < 0) {
		ret = -ENOENT;
	}
	if (ret == 0) {
		memcpy(&rec, &d->buf[last * REC_SIZE], sizeof(rec));
		spif_apply_op_point(dev, rec.frequency, true, rec.mode, rec.delay);
	} else {
		spif_point_restore(dev, &saved);
	}
	k_sem_give(&d->lock);

	return ret;
}

int sunxi_spif_params_save(const struct device *dev)
{
	const struct spif_config *cfg = dev->config;
	struct spif_data *d = dev->data;
	struct spif_point saved;
	struct spif_rec rec;
	int last, blank, ret;

	if (!cfg->has_params) {
		return -ENOENT;
	}
	if (!d->tuned) {
		return -EAGAIN;
	}
	k_sem_take(&d->lock, K_FOREVER);
	spif_point_save(dev, &saved);
	spif_rec_make(dev, &rec);
	/* with the window mapped the clock stays where it is */
	if (!d->xip) {
		spif_apply_op_point(dev, IDENT_FREQ, false, 0, 0);
	}
	ret = spif_params_scan(dev, &last, &blank);
	if (ret != 0) {
		goto out;
	}
	if (last >= 0 && memcmp(&d->buf[last * REC_SIZE], &rec, sizeof(rec)) == 0) {
		/* what is stored is what is applied */
		goto out;
	}
	if (blank < 0) {
		ret = nor_erase_block(dev, cfg->params_offset, false);
		blank = 0;
	}
	if (ret == 0) {
		memcpy(d->buf, &rec, sizeof(rec));
		ret = nor_program_page(dev, cfg->params_offset + blank * REC_SIZE, sizeof(rec));
	}
	if (ret == 0) {
		/* a protected flash ignores the program command silently */
		ret = nor_read_chunk(dev, cfg->params_offset + blank * REC_SIZE, sizeof(rec));
		if (ret == 0 && memcmp(d->buf, &rec, sizeof(rec)) != 0) {
			ret = -EIO;
		}
	}
out:
	if (!d->xip) {
		spif_point_restore(dev, &saved);
	}
	k_sem_give(&d->lock);

	return ret;
}

int sunxi_spif_params_erase(const struct device *dev)
{
	const struct spif_config *cfg = dev->config;
	struct spif_data *d = dev->data;
	struct spif_point saved;
	int ret;

	if (!cfg->has_params) {
		return -ENOENT;
	}
	k_sem_take(&d->lock, K_FOREVER);
	spif_point_save(dev, &saved);
	if (!d->xip) {
		spif_apply_op_point(dev, IDENT_FREQ, false, 0, 0);
	}
	ret = nor_erase_block(dev, cfg->params_offset, false);
	if (!d->xip) {
		spif_point_restore(dev, &saved);
	}
	k_sem_give(&d->lock);

	return ret;
}

/* ---- XIP -------------------------------------------------------------- */

int sunxi_spif_xip_enable(const struct device *dev, uint32_t flash_offset, size_t length)
{
	const struct spif_config *cfg = dev->config;
	struct spif_data *d = dev->data;
	int ret = 0;

	if ((flash_offset % SECTOR_SIZE) != 0U || length == 0U || length > cfg->xip_size ||
	    (uint64_t)flash_offset + length > d->size) {
		return -EINVAL;
	}
	k_sem_take(&d->lock, K_FOREVER);
	if (d->xip) {
		ret = -EBUSY;
		goto out;
	}
	d->xip_offset = flash_offset;
	d->xip_length = length;
	spif_window_regs(dev, &d->xip_regs);
	d->dirty_lo = UINT32_MAX;
	d->dirty_hi = 0;
	/* nothing of the window can be in the caches yet, but be sure */
	sys_cache_data_invd_range((void *)cfg->xip_base, length);
	sys_cache_instr_invd_range((void *)cfg->xip_base, length);
	spif_soft_reset(cfg);
	spif_fifo_reset(cfg);
	spif_window_on(dev, &d->xip_regs);
	d->xip = true;
out:
	k_sem_give(&d->lock);

	return ret;
}

int sunxi_spif_xip_disable(const struct device *dev)
{
	const struct spif_config *cfg = dev->config;
	struct spif_data *d = dev->data;

	k_sem_take(&d->lock, K_FOREVER);
	if (d->xip) {
		spif_window_off(dev);
		sys_cache_data_invd_range((void *)cfg->xip_base, d->xip_length);
		sys_cache_instr_invd_range((void *)cfg->xip_base, d->xip_length);
		d->xip = false;
	}
	k_sem_give(&d->lock);

	return 0;
}

const void *sunxi_spif_xip_window(const struct device *dev)
{
	const struct spif_config *cfg = dev->config;

	return (const void *)cfg->xip_base;
}

int sunxi_spif_get_info(const struct device *dev, struct sunxi_spif_info *info)
{
	const struct spif_data *d = dev->data;

	info->version = d->version;
	memcpy(info->jedec_id, d->jedec, 3);
	info->size = d->size;
	info->frequency = d->frequency;
	info->quad = d->quad;
	info->dtr = d->dtr_cap;
	info->addr_4byte = d->addr_4byte;
	info->sample_tuned = d->tuned;
	info->sample_mode = d->mode;
	info->sample_delay = d->delay;
	info->xip_active = d->xip;
	info->xip_flash_offset = d->xip_offset;
	info->xip_length = d->xip ? d->xip_length : 0U;

	return 0;
}

#ifdef CONFIG_FLASH_SUNXI_SPIF_XIP_SECTIONS
extern const uint8_t __spif_xip_load[];
extern const uint8_t __spif_xip_size[];

/* Program the xip partition with the image of the XIP sections when it differs */
static int spif_xip_deploy(const struct device *dev)
{
	const struct spif_config *cfg = dev->config;
	struct spif_data *d = dev->data;
	uint32_t size = (uint32_t)(uintptr_t)__spif_xip_size;
	bool same = true;
	int ret = 0;

	if (size > cfg->xip_length) {
		LOG_ERR("XIP sections take %u bytes, the partition has %u", size, cfg->xip_length);
		return -ENOSPC;
	}
	for (uint32_t o = 0; o < size && same && ret == 0; o += BOUNCE_SIZE) {
		uint32_t n = MIN(size - o, BOUNCE_SIZE);

		k_sem_take(&d->lock, K_FOREVER);
		ret = nor_read_chunk(dev, cfg->xip_offset + o, n);
		same = ret == 0 && memcmp(d->buf, __spif_xip_load + o, n) == 0;
		k_sem_give(&d->lock);
	}
	if (ret != 0 || same) {
		return ret;
	}
	LOG_INF("programming %u bytes of XIP code and data at %x", size, cfg->xip_offset);
	ret = spif_flash_erase(dev, cfg->xip_offset, size);
	for (uint32_t o = 0; o < size && ret == 0; o += BOUNCE_SIZE) {
		uint32_t n = MIN(size - o, BOUNCE_SIZE);

		ret = spif_flash_write(dev, cfg->xip_offset + o, __spif_xip_load + o, n);
	}

	return ret;
}
#endif

/* ---- init ------------------------------------------------------------- */

static int spif_init(const struct device *dev)
{
	const struct spif_config *cfg = dev->config;
	struct spif_data *d = dev->data;
	int ret;

	k_sem_init(&d->lock, 1, 1);
	d->dirty_lo = UINT32_MAX;

	ret = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
	if (ret < 0) {
		return ret;
	}
	ret = clock_control_on(cfg->clock_dev, (clock_control_subsys_t)(uintptr_t)cfg->bus_clock_id);
	if (ret < 0) {
		return ret;
	}
	ret = reset_line_toggle_dt(&cfg->reset);
	if (ret < 0) {
		return ret;
	}
	d->frequency = spif_set_clock(dev, IDENT_FREQ);
	spif_hw_setup(cfg);

	d->version = spif_rd(cfg, SPIF_VER);
	if (d->version < SPIF_VER_V1) {
		LOG_ERR("controller version %08x is not supported", d->version);
		return -ENOTSUP;
	}

	ret = nor_simple(dev, NOR_RDID, 3, NULL, 0);
	if (ret != 0) {
		return ret;
	}
	memcpy(d->jedec, d->buf, 3);
	if ((d->jedec[0] == 0xff && d->jedec[1] == 0xff && d->jedec[2] == 0xff) ||
	    (d->jedec[0] == 0 && d->jedec[1] == 0 && d->jedec[2] == 0)) {
		LOG_ERR("no flash answers (JEDEC ID %02x%02x%02x)", d->jedec[0], d->jedec[1],
			d->jedec[2]);
		return -ENODEV;
	}
	if (cfg->has_jedec && memcmp(cfg->jedec, d->jedec, 3) != 0) {
		LOG_ERR("JEDEC ID %02x%02x%02x, expected %02x%02x%02x", d->jedec[0], d->jedec[1],
			d->jedec[2], cfg->jedec[0], cfg->jedec[1], cfg->jedec[2]);
		return -ENODEV;
	}

	d->size = cfg->size != 0U ? cfg->size : BIT(d->jedec[2]);
	d->addr_4byte = cfg->addr_4byte || d->size > BIT(24);
	d->layout.pages_count = d->size / SECTOR_SIZE;
	d->layout.pages_size = SECTOR_SIZE;
	LOG_INF("flash %02x%02x%02x, %u KiB, controller %08x", d->jedec[0], d->jedec[1],
		d->jedec[2], d->size / 1024U, d->version);

	if (cfg->quad) {
		if (nor_enable_quad(dev) == 0) {
			d->quad = true;
		} else {
			LOG_WRN("quad enable failed, using one wire");
		}
	}

	{
		uint8_t sr1 = 0, sr2 = 0;

		if (nor_read_status(dev, NOR_RDSR, &sr1) == 0) {
			(void)nor_read_status(dev, NOR_RDSR2, &sr2);
		}
		LOG_INF("status registers %02x %02x", sr1, sr2);
		if (cfg->clear_bp) {
			ret = nor_clear_protect(dev, &sr1);
			if (ret != 0) {
				LOG_WRN("could not clear the block protect bits: %d", ret);
			} else {
				(void)nor_read_status(dev, NOR_RDSR2, &sr2);
				LOG_INF("status registers now %02x %02x", sr1, sr2);
			}
		}
	}

	d->tuned = false;
	spif_dtr_probe(dev);
	if (cfg->frequency > IDENT_FREQ) {
		ret = cfg->has_params ? sunxi_spif_params_load(dev) : -ENOENT;
		if (ret == 0) {
			LOG_INF("sample point from the params partition: mode %u delay %u, %u Hz",
				d->mode, d->delay, d->frequency);
		} else if (IS_ENABLED(CONFIG_FLASH_SUNXI_SPIF_TUNE_AT_INIT)) {
			struct sunxi_spif_tune_result res;

			ret = sunxi_spif_tune(dev, &res);
			if (ret == 0) {
				LOG_INF("tuned: mode %u delay %u (window %u..%u), %u Hz", res.mode,
					res.delay, res.window_start,
					res.window_start + res.window_len - 1U, d->frequency);
				if (cfg->has_params && sunxi_spif_params_save(dev) != 0) {
					LOG_WRN("could not save the sample point");
				}
			} else {
				LOG_WRN("tuning failed (%d), running at %u Hz", ret, d->frequency);
			}
		}
	}

	if (cfg->xip_at_init) {
		uint32_t length = cfg->xip_length;

#ifdef CONFIG_FLASH_SUNXI_SPIF_XIP_SECTIONS
		ret = spif_xip_deploy(dev);
		if (ret != 0) {
			LOG_ERR("could not deploy the XIP sections: %d", ret);
			return ret;
		}
		length = MIN(length, (uint32_t)(uintptr_t)__spif_xip_size);
#endif
		if (length != 0U) {
			ret = sunxi_spif_xip_enable(dev, cfg->xip_offset, length);
			if (ret != 0) {
				LOG_WRN("xip enable failed: %d", ret);
			}
		}
	}

	return 0;
}

static DEVICE_API(flash, spif_flash_api) = {
	.read = spif_flash_read,
	.write = spif_flash_write,
	.erase = spif_flash_erase,
	.get_parameters = spif_flash_get_parameters,
	.get_size = spif_flash_get_size,
#if defined(CONFIG_FLASH_PAGE_LAYOUT)
	.page_layout = spif_flash_page_layout,
#endif
};

#define SPIF_PART(inst, prop) DT_NODELABEL(DT_INST_STRING_TOKEN(inst, prop))

#define SPIF_INIT(inst)								\
	BUILD_ASSERT(!IS_ENABLED(CONFIG_FLASH_SUNXI_SPIF_XIP_SECTIONS) ||	\
		     DT_INST_REG_ADDR_BY_NAME(inst, xip) ==			\
		     CONFIG_FLASH_SUNXI_SPIF_XIP_BASE,				\
		     "FLASH_SUNXI_SPIF_XIP_BASE is not the base of the xip window");	\
	PINCTRL_DT_INST_DEFINE(inst);						\
	static struct spif_data spif_data_##inst __aligned(64);			\
	static const struct spif_config spif_config_##inst = {			\
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(inst),			\
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(inst)),		\
		.base = DT_INST_REG_ADDR_BY_NAME(inst, ctrl),			\
		.xip_base = DT_INST_REG_ADDR_BY_NAME(inst, xip),		\
		.xip_size = DT_INST_REG_SIZE_BY_NAME(inst, xip),		\
		.bus_clock_id = DT_INST_CLOCKS_CELL_BY_NAME(inst, bus, clkid),	\
		.reset = RESET_DT_SPEC_INST_GET(inst),				\
		.clock_reg = DT_INST_PROP(inst, clock_reg),			\
		.frequency = DT_INST_PROP(inst, clock_frequency),		\
		.size = DT_INST_PROP_OR(inst, size, 0) / 8,			\
		.has_jedec = DT_INST_NODE_HAS_PROP(inst, jedec_id),		\
		.jedec = COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, jedec_id),	\
				     (DT_INST_PROP(inst, jedec_id)), ({ 0 })),	\
		.quad = DT_INST_PROP(inst, quad),				\
		.dtr = DT_INST_PROP(inst, dtr),					\
		.dtr_opcode = DT_INST_PROP(inst, dtr_read_opcode),		\
		.dtr_dummy = DT_INST_PROP(inst, dtr_read_dummy),		\
		.addr_4byte = DT_INST_PROP(inst, address_4byte),		\
		.clear_bp = DT_INST_PROP(inst, clear_block_protect),		\
		.has_params = DT_INST_NODE_HAS_PROP(inst, params_partition),	\
		.params_offset = COND_CODE_1(					\
			DT_INST_NODE_HAS_PROP(inst, params_partition),		\
			(DT_REG_ADDR(SPIF_PART(inst, params_partition))), (0)),	\
		.params_size = COND_CODE_1(					\
			DT_INST_NODE_HAS_PROP(inst, params_partition),		\
			(DT_REG_SIZE(SPIF_PART(inst, params_partition))), (0)),	\
		.tune_offset = COND_CODE_1(					\
			DT_INST_NODE_HAS_PROP(inst, tune_partition),		\
			(DT_REG_ADDR(SPIF_PART(inst, tune_partition))), (0)),	\
		.xip_at_init = IS_ENABLED(CONFIG_FLASH_SUNXI_SPIF_XIP_AT_INIT) &&	\
			       DT_INST_NODE_HAS_PROP(inst, xip_partition),	\
		.xip_offset = COND_CODE_1(					\
			DT_INST_NODE_HAS_PROP(inst, xip_partition),		\
			(DT_REG_ADDR(SPIF_PART(inst, xip_partition))), (0)),	\
		.xip_length = COND_CODE_1(					\
			DT_INST_NODE_HAS_PROP(inst, xip_partition),		\
			(DT_REG_SIZE(SPIF_PART(inst, xip_partition))), (0)),	\
	};									\
	DEVICE_DT_INST_DEFINE(inst, spif_init, NULL, &spif_data_##inst,		\
			      &spif_config_##inst, POST_KERNEL,			\
			      CONFIG_FLASH_SUNXI_SPIF_INIT_PRIORITY, &spif_flash_api);

DT_INST_FOREACH_STATUS_OKAY(SPIF_INIT)

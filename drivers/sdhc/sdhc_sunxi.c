/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Allwinner sunxi SD/MMC host controller (SMHC).
 *
 * The controller shifts commands and data through a 32 bit FIFO that is
 * either drained by the CPU or by the internal DMA engine (IDMAC). The
 * IDMAC walks a chain of 16 byte descriptors, each pointing at up to 8 KiB.
 * A request is started with a single write to the command register and
 * completes in the interrupt handler; the requesting thread sleeps on a
 * semaphore in the meantime.
 *
 * The module clock runs at twice the card clock (four times for DDR) and the
 * card clock is derived from it by the controller; sampling and drive phases
 * depend on the card clock and are selected by frequency range.
 */

#define DT_DRV_COMPAT allwinner_sunxi_smhc

#include <errno.h>
#include <string.h>
#include <zephyr/cache.h>
#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/reset.h>
#include <zephyr/drivers/sdhc.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sd/sd_spec.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(sdhc_sunxi, CONFIG_SDHC_LOG_LEVEL);

/* Registers */
#define SMHC_GCTRL	0x00
#define SMHC_CLKCR	0x04
#define SMHC_TMOUT	0x08
#define SMHC_WIDTH	0x0c
#define SMHC_BLKSZ	0x10
#define SMHC_BCNTR	0x14
#define SMHC_CMDR	0x18
#define SMHC_CARG	0x1c
#define SMHC_RESP0	0x20
#define SMHC_IMASK	0x30
#define SMHC_MISTA	0x34
#define SMHC_RINTR	0x38
#define SMHC_STAS	0x3c
#define SMHC_FTRGL	0x40
#define SMHC_A12A	0x58
#define SMHC_NTSR	0x5c
#define SMHC_DMAC	0x80
#define SMHC_DLBA	0x84
#define SMHC_IDST	0x88
#define SMHC_IDIE	0x8c
#define SMHC_DRV_DL	0x140
#define SMHC_FIFO	0x200

/* SMHC_GCTRL */
#define GCTRL_SOFT_RST		BIT(0)
#define GCTRL_FIFO_RST		BIT(1)
#define GCTRL_DMA_RST		BIT(2)
#define GCTRL_ALL_RST		(GCTRL_SOFT_RST | GCTRL_FIFO_RST | GCTRL_DMA_RST)
#define GCTRL_INT_EN		BIT(4)
#define GCTRL_DMA_EN		BIT(5)
#define GCTRL_DDR_MODE		BIT(10)
#define GCTRL_ACCESS_DONE_DIR	BIT(30)
#define GCTRL_ACCESS_BY_AHB	BIT(31)

/* SMHC_CLKCR */
#define CLKCR_DIV_MASK		GENMASK(7, 0)
#define CLKCR_CARD_CLK_ON	BIT(16)
#define CLKCR_LOW_POWER		BIT(17)
#define CLKCR_MASK_DATA0	BIT(31)

/* SMHC_CMDR */
#define CMDR_OPCODE_MASK	GENMASK(5, 0)
#define CMDR_RSP_EXP		BIT(6)
#define CMDR_LONG_RSP		BIT(7)
#define CMDR_CHECK_CRC		BIT(8)
#define CMDR_DATA_EXP		BIT(9)
#define CMDR_WRITE		BIT(10)
#define CMDR_AUTO_STOP		BIT(12)
#define CMDR_WAIT_PRE_OVER	BIT(13)
#define CMDR_SEND_INIT_SEQ	BIT(15)
#define CMDR_UPCLK_ONLY		BIT(21)
#define CMDR_START		BIT(31)

/* SMHC_RINTR / SMHC_MISTA / SMHC_IMASK */
#define INT_RESP_ERR		BIT(1)
#define INT_CMD_DONE		BIT(2)
#define INT_DATA_OVER		BIT(3)
#define INT_RESP_CRC_ERR	BIT(6)
#define INT_DATA_CRC_ERR	BIT(7)
#define INT_RESP_TIMEOUT	BIT(8)
#define INT_DATA_TIMEOUT	BIT(9)
#define INT_FIFO_RUN_ERR	BIT(11)
#define INT_HARD_LOCKED		BIT(12)
#define INT_START_BIT_ERR	BIT(13)
#define INT_AUTO_CMD_DONE	BIT(14)
#define INT_END_BIT_ERR		BIT(15)
#define INT_SDIO		BIT(16)
#define INT_ERR_MASK		(INT_RESP_ERR | INT_RESP_CRC_ERR | INT_DATA_CRC_ERR |	\
				 INT_RESP_TIMEOUT | INT_DATA_TIMEOUT | INT_FIFO_RUN_ERR |	\
				 INT_HARD_LOCKED | INT_START_BIT_ERR | INT_END_BIT_ERR)

/* SMHC_STAS */
#define STAS_FIFO_EMPTY		BIT(2)
#define STAS_FIFO_FULL		BIT(3)
#define STAS_CARD_BUSY		BIT(9)

/* SMHC_DMAC */
#define DMAC_SOFT_RST		BIT(0)
#define DMAC_FIX_BURST		BIT(1)
#define DMAC_ON			BIT(7)

/* SMHC_IDST / SMHC_IDIE */
#define IDST_TX_INT		BIT(0)
#define IDST_RX_INT		BIT(1)
#define IDST_FATAL_BUS_ERR	BIT(2)
#define IDST_DES_INVALID	BIT(4)
#define IDST_CARD_ERR_SUM	BIT(5)
#define IDST_ABNORMAL_SUM	BIT(9)
#define IDST_ERR_MASK		(IDST_FATAL_BUS_ERR | IDST_DES_INVALID | IDST_CARD_ERR_SUM | \
				 IDST_ABNORMAL_SUM)
#define IDST_ALL		0x337

/* SMHC_NTSR / SMHC_DRV_DL */
#define NTSR_CMD_PH_MASK	GENMASK(5, 4)
#define NTSR_DAT_PH_MASK	GENMASK(9, 8)
#define NTSR_2X_TIMING		BIT(31)
#define DRV_DL_CMD_PH		BIT(16)
#define DRV_DL_DAT_PH		BIT(17)

/* IDMAC descriptor */
#define DES_CFG_DIC		BIT(1)
#define DES_CFG_LD		BIT(2)
#define DES_CFG_FD		BIT(3)
#define DES_CFG_CH		BIT(4)
#define DES_CFG_ER		BIT(5)
#define DES_CFG_OWN		BIT(31)
#define DES_MAX_LEN		8192U
#define DES_ADDR_SHIFT		2

/* Module clock register */
#define MOD_CLK_GATE		BIT(31)
#define MOD_CLK_MUX_SHIFT	24
#define MOD_CLK_MUX_HOSC	0U
#define MOD_CLK_MUX_PLL_1X	1U
#define MOD_CLK_P_SHIFT		8
#define MOD_CLK_M_MASK		GENMASK(3, 0)
#define MOD_CLK_P_MASK		GENMASK(9, 8)

#define CCU_BASE		0x02001000U
#define PLL_PERI_REG		0x0020U
#define HOSC_RATE		24000000U

#define SMHC_RESET_TIMEOUT_US	100000
#define SMHC_CLK_UPDATE_TIMEOUT_US 100000
#define SMHC_FIFO_TIMEOUT_MS	1000
#define SMHC_BUSY_TIMEOUT_MS	2000
#define SMHC_DMA_ALIGN		CONFIG_DCACHE_LINE_SIZE
#define SMHC_RX_WATERMARK	7
#define SMHC_TX_WATERMARK	248
#define SMHC_BURST_SIZE		2
/* transfers below this size are copied by the CPU when the buffer is not aligned */
#define SMHC_BOUNCE_MIN		512U

struct smhc_desc {
	uint32_t config;
	uint32_t size;
	uint32_t buf_addr;
	uint32_t next_addr;
};

struct smhc_config {
	uintptr_t base;
	uint32_t clock_reg;
	const struct device *clock_dev;
	clock_control_subsys_t bus_clk;
	struct reset_dt_spec reset;
	const struct pinctrl_dev_config *pcfg;
	struct gpio_dt_spec cd;
	struct gpio_dt_spec pwr;
	void (*irq_config)(void);
	uint32_t f_min;
	uint32_t f_max;
	uint32_t power_delay;
	uint8_t bus_width;
	bool non_removable;
};

struct smhc_data {
	struct k_mutex lock;
	struct k_sem done;
	struct sdhc_io ios;
	/* allocated when first needed */
	uint8_t *bounce;

	/* Request state shared with the interrupt handler */
	volatile uint32_t need;
	volatile uint32_t rint;
	volatile uint32_t idst;
	uint32_t need_idst;
	bool active;

	/* Callbacks */
	sdhc_interrupt_cb_t cb;
	void *cb_data;
	int cb_sources;
	uint32_t sdio_mask;
	struct gpio_callback cd_cb;
	const struct device *dev;

	struct smhc_desc desc[CONFIG_SDHC_SUNXI_MAX_DESC] __aligned(SMHC_DMA_ALIGN);
};

static inline uint32_t smhc_rd(const struct device *dev, uint32_t off)
{
	const struct smhc_config *cfg = dev->config;

	return sys_read32(cfg->base + off);
}

static inline void smhc_wr(const struct device *dev, uint32_t off, uint32_t val)
{
	const struct smhc_config *cfg = dev->config;

	sys_write32(val, cfg->base + off);
}

static int smhc_wait_clear(const struct device *dev, uint32_t off, uint32_t mask)
{
	for (int i = 0; i < SMHC_RESET_TIMEOUT_US; i++) {
		if ((smhc_rd(dev, off) & mask) == 0U) {
			return 0;
		}
		k_busy_wait(1);
	}

	return -ETIMEDOUT;
}

/* ---- clock ---------------------------------------------------------------------------- */

static uint32_t smhc_pll_peri_1x_rate(void)
{
	uint32_t pll = sys_read32(CCU_BASE + PLL_PERI_REG);
	uint32_t n = ((pll >> 8) & 0xff) + 1;
	uint32_t p0 = ((pll >> 16) & 0x7) + 1;
	uint32_t m = (pll & BIT(1)) ? 2 : 1;

	/* PLL_PERI_1X is half of PLL_PERI_2X */
	return HOSC_RATE / m / p0 * n / 2;
}

/* Latch a new clock configuration into the card interface */
static int smhc_update_clock(const struct device *dev)
{
	uint32_t clkcr = smhc_rd(dev, SMHC_CLKCR);
	int ret;

	smhc_wr(dev, SMHC_CLKCR, clkcr | CLKCR_MASK_DATA0);
	smhc_wr(dev, SMHC_CMDR, CMDR_START | CMDR_UPCLK_ONLY | CMDR_WAIT_PRE_OVER);
	ret = smhc_wait_clear(dev, SMHC_CMDR, CMDR_START);
	smhc_wr(dev, SMHC_RINTR, 0xffffffffU);
	smhc_wr(dev, SMHC_CLKCR, clkcr & ~CLKCR_MASK_DATA0);

	return ret;
}

static void smhc_set_phase(const struct device *dev, uint32_t hz)
{
	uint32_t drv = smhc_rd(dev, SMHC_DRV_DL);
	uint32_t ntsr = smhc_rd(dev, SMHC_NTSR);
	uint32_t sample;

	/* Command drive is always shifted by half a cycle, data only above 26 MHz */
	drv |= DRV_DL_CMD_PH;
	if (hz > 26000000U && hz <= 52000000U) {
		drv |= DRV_DL_DAT_PH;
		sample = 1;
	} else {
		drv &= ~DRV_DL_DAT_PH;
		sample = 0;
	}

	smhc_wr(dev, SMHC_DRV_DL, drv);
	ntsr &= ~(NTSR_CMD_PH_MASK | NTSR_DAT_PH_MASK);
	ntsr |= FIELD_PREP(NTSR_CMD_PH_MASK, sample) | FIELD_PREP(NTSR_DAT_PH_MASK, sample);
	ntsr |= NTSR_2X_TIMING;
	smhc_wr(dev, SMHC_NTSR, ntsr);
}

static int smhc_set_clock(const struct device *dev, uint32_t hz, bool ddr)
{
	const struct smhc_config *cfg = dev->config;
	uint32_t clkcr = smhc_rd(dev, SMHC_CLKCR) & ~(CLKCR_CARD_CLK_ON | CLKCR_LOW_POWER);
	uint32_t gctrl;
	int ret;

	/* Stop the card clock before touching the module clock */
	smhc_wr(dev, SMHC_CLKCR, clkcr);
	ret = smhc_update_clock(dev);
	if (ret || hz == 0U) {
		return ret;
	}

	uint32_t target = hz * (ddr ? 4U : 2U);
	uint32_t src, mux, div, p, m;

	if (hz > HOSC_RATE / 2U) {
		src = smhc_pll_peri_1x_rate();
		mux = MOD_CLK_MUX_PLL_1X;
	} else {
		src = HOSC_RATE;
		mux = MOD_CLK_MUX_HOSC;
	}

	div = CLAMP((src + target / 2U) / target, 1U, 128U);
	if (div > 64U) {
		p = 3;
	} else if (div > 32U) {
		p = 2;
	} else if (div > 16U) {
		p = 1;
	} else {
		p = 0;
	}
	m = DIV_ROUND_UP(div, 1U << p);

	/* Reprogram the module clock with its gate closed */
	sys_write32((mux << MOD_CLK_MUX_SHIFT) | (p << MOD_CLK_P_SHIFT) | (m - 1U),
		    CCU_BASE + cfg->clock_reg);

	gctrl = smhc_rd(dev, SMHC_GCTRL);
	if (ddr) {
		gctrl |= GCTRL_DDR_MODE;
	} else {
		gctrl &= ~GCTRL_DDR_MODE;
	}
	smhc_wr(dev, SMHC_GCTRL, gctrl);

	sys_write32((mux << MOD_CLK_MUX_SHIFT) | (p << MOD_CLK_P_SHIFT) | (m - 1U) | MOD_CLK_GATE,
		    CCU_BASE + cfg->clock_reg);

	clkcr &= ~CLKCR_DIV_MASK;
	if (ddr) {
		clkcr |= 1U;
	}
	smhc_wr(dev, SMHC_CLKCR, clkcr);

	smhc_set_phase(dev, hz);

	LOG_DBG("clock %u Hz: src %u mux %u div %u (p %u m %u)", hz, src, mux, div, p, m);

	smhc_wr(dev, SMHC_CLKCR, clkcr | CLKCR_CARD_CLK_ON);
	return smhc_update_clock(dev);
}

/* ---- interrupt ------------------------------------------------------------------------ */

static void smhc_isr(const struct device *dev)
{
	struct smhc_data *data = dev->data;
	uint32_t msk = smhc_rd(dev, SMHC_MISTA);
	uint32_t idst = smhc_rd(dev, SMHC_IDST);
	uint32_t rint;

	if (msk & INT_SDIO) {
		/* The card holds the interrupt until it is serviced: mask it until re-enabled */
		smhc_wr(dev, SMHC_RINTR, INT_SDIO);
		data->sdio_mask = 0;
		smhc_wr(dev, SMHC_IMASK, smhc_rd(dev, SMHC_IMASK) & ~INT_SDIO);
		if (data->cb != NULL && (data->cb_sources & SDHC_INT_SDIO)) {
			data->cb(dev, SDHC_INT_SDIO, data->cb_data);
		}
		msk &= ~INT_SDIO;
	}

	smhc_wr(dev, SMHC_RINTR, msk);
	smhc_wr(dev, SMHC_IDST, idst);

	if (!data->active) {
		return;
	}

	data->rint |= msk;
	data->idst |= idst;
	rint = data->rint;

	if ((rint & INT_ERR_MASK) || (data->idst & IDST_ERR_MASK) ||
	    ((rint & data->need) == data->need && (data->idst & data->need_idst) == data->need_idst)) {
		smhc_wr(dev, SMHC_IMASK, data->sdio_mask);
		k_sem_give(&data->done);
	}
}

/* ---- request -------------------------------------------------------------------------- */

static int smhc_reset_ctrl(const struct device *dev)
{
	struct smhc_data *data = dev->data;
	int ret;

	smhc_wr(dev, SMHC_GCTRL, smhc_rd(dev, SMHC_GCTRL) | GCTRL_ALL_RST | GCTRL_INT_EN |
	       GCTRL_ACCESS_DONE_DIR);
	ret = smhc_wait_clear(dev, SMHC_GCTRL, GCTRL_ALL_RST);
	smhc_wr(dev, SMHC_RINTR, 0xffffffffU);
	smhc_wr(dev, SMHC_IDST, IDST_ALL);
	smhc_wr(dev, SMHC_IMASK, data->sdio_mask);
	/* response timeout 255 card clocks, data timeout 16M card clocks */
	smhc_wr(dev, SMHC_TMOUT, (0xffffffU << 8) | 0xffU);

	return ret;
}

/* Bring the controller back to a known state after a failed request */
static void smhc_recover(const struct device *dev)
{
	struct smhc_data *data = dev->data;
	struct sdhc_io ios = data->ios;

	smhc_reset_ctrl(dev);
	smhc_wr(dev, SMHC_WIDTH, ios.bus_width == SDHC_BUS_WIDTH8BIT ? 2U :
				  ios.bus_width == SDHC_BUS_WIDTH4BIT ? 1U : 0U);
	smhc_set_clock(dev, ios.clock, ios.timing == SDHC_TIMING_DDR52 ||
				       ios.timing == SDHC_TIMING_DDR50);
}

static uint32_t smhc_cmd_flags(const struct sdhc_command *cmd)
{
	uint32_t flags = CMDR_START | (cmd->opcode & CMDR_OPCODE_MASK);

	switch (cmd->response_type & SDHC_NATIVE_RESPONSE_MASK) {
	case SD_RSP_TYPE_NONE:
		break;
	case SD_RSP_TYPE_R2:
		flags |= CMDR_RSP_EXP | CMDR_LONG_RSP | CMDR_CHECK_CRC;
		break;
	case SD_RSP_TYPE_R3:
	case SD_RSP_TYPE_R4:
		flags |= CMDR_RSP_EXP;
		break;
	default:
		flags |= CMDR_RSP_EXP | CMDR_CHECK_CRC;
		break;
	}

	if (cmd->opcode == SD_GO_IDLE_STATE) {
		flags |= CMDR_SEND_INIT_SEQ;
	}

	return flags;
}

/* The host is not told the data direction, it follows from the command */
static bool smhc_cmd_writes(const struct sdhc_command *cmd)
{
	switch (cmd->opcode) {
	case SD_WRITE_SINGLE_BLOCK:
	case SD_WRITE_MULTIPLE_BLOCK:
	case MMC_SEND_BUS_TEST:
		return true;
	case SDIO_RW_EXTENDED:
		return (cmd->arg & BIT(31)) != 0U;
	default:
		return false;
	}
}

static int smhc_build_desc(struct smhc_data *data, const void *buf, uint32_t len)
{
	uint32_t n = DIV_ROUND_UP(len, DES_MAX_LEN);
	uintptr_t addr = (uintptr_t)buf;

	if (n > ARRAY_SIZE(data->desc)) {
		return -EINVAL;
	}

	for (uint32_t i = 0; i < n; i++) {
		struct smhc_desc *d = &data->desc[i];
		uint32_t chunk = MIN(len - i * DES_MAX_LEN, DES_MAX_LEN);
		uint32_t cfg = DES_CFG_CH | DES_CFG_OWN | DES_CFG_DIC;

		if (i == 0) {
			cfg |= DES_CFG_FD;
		}
		d->size = chunk;
		d->buf_addr = (addr + i * DES_MAX_LEN) >> DES_ADDR_SHIFT;
		if (i == n - 1) {
			cfg = (cfg & ~DES_CFG_DIC) | DES_CFG_LD | DES_CFG_ER;
			d->next_addr = 0;
		} else {
			d->next_addr = (uintptr_t)&data->desc[i + 1] >> DES_ADDR_SHIFT;
		}
		d->config = cfg;
	}

	sys_cache_data_flush_range(data->desc, n * sizeof(struct smhc_desc));
	return 0;
}

static void smhc_dma_start(const struct device *dev, bool read)
{
	struct smhc_data *data = dev->data;
	uint32_t gctrl = smhc_rd(dev, SMHC_GCTRL) & ~GCTRL_ACCESS_BY_AHB;

	smhc_wr(dev, SMHC_GCTRL, gctrl | GCTRL_DMA_EN);
	smhc_wr(dev, SMHC_GCTRL, gctrl | GCTRL_DMA_EN | GCTRL_DMA_RST | GCTRL_FIFO_RST);
	smhc_wait_clear(dev, SMHC_GCTRL, GCTRL_DMA_RST | GCTRL_FIFO_RST);
	smhc_wr(dev, SMHC_DMAC, DMAC_SOFT_RST);
	smhc_wait_clear(dev, SMHC_DMAC, DMAC_SOFT_RST);
	smhc_wr(dev, SMHC_DMAC, DMAC_FIX_BURST | DMAC_ON);
	smhc_wr(dev, SMHC_IDIE, read ? IDST_RX_INT : 0U);
	smhc_wr(dev, SMHC_DLBA, (uintptr_t)data->desc >> DES_ADDR_SHIFT);
	smhc_wr(dev, SMHC_FTRGL, (SMHC_BURST_SIZE << 28) | (SMHC_RX_WATERMARK << 16) |
				 SMHC_TX_WATERMARK);
}

static void smhc_dma_stop(const struct device *dev)
{
	smhc_wr(dev, SMHC_IDST, IDST_ALL);
	smhc_wr(dev, SMHC_IDIE, 0);
	smhc_wr(dev, SMHC_DMAC, 0);
	smhc_wr(dev, SMHC_GCTRL, (smhc_rd(dev, SMHC_GCTRL) | GCTRL_DMA_RST) & ~GCTRL_DMA_EN);
}

/* Wait until the FIFO status flag (empty when reading, full when writing) clears */
static int smhc_fifo_wait(const struct device *dev, uint32_t flag)
{
	int64_t end = k_uptime_get() + SMHC_FIFO_TIMEOUT_MS;

	while ((smhc_rd(dev, SMHC_STAS) & flag) != 0U) {
		if (k_uptime_get() > end) {
			return -ETIMEDOUT;
		}
		if (smhc_rd(dev, SMHC_RINTR) & INT_ERR_MASK) {
			return -EIO;
		}
	}

	return 0;
}

/* CPU copy through the FIFO, used for small or unaligned buffers */
static int smhc_pio(const struct device *dev, uint8_t *buf, uint32_t len, bool write)
{
	uint32_t i = 0;

	while (i < len) {
		uint32_t w = 0;
		uint32_t n = MIN(len - i, 4U);
		int ret = smhc_fifo_wait(dev, write ? STAS_FIFO_FULL : STAS_FIFO_EMPTY);

		if (ret) {
			return ret;
		}

		if (write) {
			memcpy(&w, buf + i, n);
			smhc_wr(dev, SMHC_FIFO, w);
		} else {
			w = smhc_rd(dev, SMHC_FIFO);
			memcpy(buf + i, &w, n);
		}
		i += n;
	}

	return 0;
}

static int smhc_check_errors(uint32_t rint, uint32_t idst)
{
	if (rint & (INT_RESP_TIMEOUT | INT_DATA_TIMEOUT)) {
		return -ETIMEDOUT;
	}
	if ((rint & INT_ERR_MASK) || (idst & IDST_ERR_MASK)) {
		return -EIO;
	}

	return 0;
}

static int smhc_wait_idle(const struct device *dev, int timeout_ms)
{
	int64_t end = k_uptime_get() + timeout_ms;

	while (smhc_rd(dev, SMHC_STAS) & STAS_CARD_BUSY) {
		if (k_uptime_get() > end) {
			return -EBUSY;
		}
		k_msleep(1);
	}

	return 0;
}

/* Aligned memory for transfers whose own buffer cannot be used by the DMA engine */
static void *smhc_bounce(struct smhc_data *data, uint32_t len)
{
	if (CONFIG_SDHC_SUNXI_BOUNCE_KB == 0 || len > CONFIG_SDHC_SUNXI_BOUNCE_KB * 1024U) {
		return NULL;
	}
	if (data->bounce == NULL) {
		data->bounce = k_aligned_alloc(SMHC_DMA_ALIGN, CONFIG_SDHC_SUNXI_BOUNCE_KB * 1024U);
		if (data->bounce == NULL) {
			LOG_WRN("no memory for the bounce buffer");
		}
	}

	return data->bounce;
}

static int smhc_xfer(const struct device *dev, struct sdhc_command *cmd, struct sdhc_data *sd)
{
	struct smhc_data *data = dev->data;
	uint32_t cmdr = smhc_cmd_flags(cmd);
	uint32_t imask = INT_ERR_MASK | data->sdio_mask;
	bool use_dma = false;
	bool write = false;
	/* memory the DMA engine moves data to or from: the caller's or the bounce buffer */
	void *dma_buf = NULL;
	uint32_t len = 0;
	int timeout = cmd->timeout_ms > 0 ? cmd->timeout_ms : 1000;
	int ret;

	ret = smhc_wait_idle(dev, SMHC_BUSY_TIMEOUT_MS);
	if (ret) {
		LOG_ERR("card busy before CMD%u", cmd->opcode);
		return ret;
	}

	if (smhc_rd(dev, SMHC_GCTRL) & GCTRL_ALL_RST) {
		return -EBUSY;
	}

	data->need = 0;
	data->need_idst = 0;
	data->rint = 0;
	data->idst = 0;
	k_sem_reset(&data->done);
	smhc_wr(dev, SMHC_RINTR, 0xffffffffU);
	smhc_wr(dev, SMHC_IDST, IDST_ALL);

	if (sd != NULL) {
		len = sd->block_size * sd->blocks;
		write = smhc_cmd_writes(cmd);
		timeout += sd->timeout_ms;

		smhc_wr(dev, SMHC_BLKSZ, sd->block_size);
		smhc_wr(dev, SMHC_BCNTR, len);

		cmdr |= CMDR_DATA_EXP | CMDR_WAIT_PRE_OVER;
		if (write) {
			cmdr |= CMDR_WRITE;
		}

		if (cmd->opcode == SD_READ_MULTIPLE_BLOCK || cmd->opcode == SD_WRITE_MULTIPLE_BLOCK) {
			cmdr |= CMDR_AUTO_STOP;
			data->need = INT_AUTO_CMD_DONE;
		} else {
			data->need = INT_DATA_OVER;
		}

		/*
		 * The engine needs cache-line aligned memory (the lines around a buffer
		 * are not ours to invalidate). Other buffers of block size or more go
		 * through an aligned bounce buffer; only small odd ones are copied by
		 * the CPU.
		 */
		if (len > 4U && len <= ARRAY_SIZE(data->desc) * DES_MAX_LEN) {
			if (((uintptr_t)sd->data % SMHC_DMA_ALIGN) == 0U &&
			    (len % SMHC_DMA_ALIGN) == 0U) {
				dma_buf = sd->data;
			} else if (len >= SMHC_BOUNCE_MIN) {
				dma_buf = smhc_bounce(data, len);
				if (dma_buf != NULL && write) {
					memcpy(dma_buf, sd->data, len);
				}
			}
		}
		use_dma = dma_buf != NULL;
		if (use_dma) {
			sys_cache_data_flush_range(dma_buf, ROUND_UP(len, SMHC_DMA_ALIGN));
			ret = smhc_build_desc(data, dma_buf, len);
			if (ret) {
				return ret;
			}
			smhc_dma_start(dev, !write);
			if (!write) {
				data->need_idst = IDST_RX_INT;
			}
		} else {
			smhc_wr(dev, SMHC_GCTRL, smhc_rd(dev, SMHC_GCTRL) | GCTRL_ACCESS_BY_AHB |
							 GCTRL_FIFO_RST);
			smhc_wait_clear(dev, SMHC_GCTRL, GCTRL_FIFO_RST);
		}
		imask |= data->need;
	} else {
		data->need = INT_CMD_DONE;
		imask |= INT_CMD_DONE;
	}

	smhc_wr(dev, SMHC_A12A, (cmdr & CMDR_AUTO_STOP) ? 0U : 0xffffU);
	smhc_wr(dev, SMHC_CARG, cmd->arg);
	data->active = true;
	smhc_wr(dev, SMHC_IMASK, imask);
	smhc_wr(dev, SMHC_CMDR, cmdr);

	if (sd != NULL && !use_dma) {
		ret = smhc_pio(dev, sd->data, len, write);
		if (ret) {
			goto out;
		}
	}

	ret = k_sem_take(&data->done, K_MSEC(timeout));
	if (ret) {
		ret = -ETIMEDOUT;
		LOG_ERR("CMD%u wait timeout (rint %08x idst %08x need %08x)", cmd->opcode, data->rint,
			data->idst, data->need);
	}

out:
	data->active = false;
	smhc_wr(dev, SMHC_IMASK, data->sdio_mask);

	if (ret == 0) {
		ret = smhc_check_errors(data->rint, data->idst);
	}
	if (ret == 0 && data->need != INT_CMD_DONE) {
		/* the data phase has to be complete when the transfer is declared done */
		if ((data->rint & data->need) != data->need) {
			ret = -EIO;
		}
	}

	if (sd != NULL) {
		if (use_dma) {
			smhc_dma_stop(dev);
			if (!write) {
				sys_cache_data_invd_range(dma_buf, ROUND_UP(len, SMHC_DMA_ALIGN));
				if (dma_buf != sd->data && ret == 0) {
					memcpy(sd->data, dma_buf, len);
				}
			}
		}
		smhc_wr(dev, SMHC_GCTRL, smhc_rd(dev, SMHC_GCTRL) | GCTRL_FIFO_RST);
	}

	if (ret == 0) {
		if ((cmd->response_type & SDHC_NATIVE_RESPONSE_MASK) == SD_RSP_TYPE_R2) {
			for (int i = 0; i < 4; i++) {
				cmd->response[i] = smhc_rd(dev, SMHC_RESP0 + 4 * i);
			}
		} else {
			cmd->response[0] = smhc_rd(dev, SMHC_RESP0);
		}
		if (sd != NULL) {
			sd->bytes_xfered = len;
		}
	} else {
		LOG_DBG("CMD%u failed: %d (rint %08x idst %08x)", cmd->opcode, ret, data->rint,
			data->idst);
		/* a command that merely got no answer leaves the controller usable */
		if (sd != NULL || ret != -ETIMEDOUT || (smhc_rd(dev, SMHC_CMDR) & CMDR_START)) {
			smhc_recover(dev);
		}
	}

	return ret;
}

static int smhc_request(const struct device *dev, struct sdhc_command *cmd, struct sdhc_data *sd)
{
	struct smhc_data *data = dev->data;
	int retries = cmd->retries;
	int ret;

	if (sd != NULL && (sd->block_size == 0U || sd->blocks == 0U || sd->data == NULL)) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	do {
		ret = smhc_xfer(dev, cmd, sd);
		if (ret != -EIO) {
			break;
		}
	} while (retries-- > 0);
	k_mutex_unlock(&data->lock);

	return ret;
}

/* ---- host API ------------------------------------------------------------------------- */

static int smhc_hw_reset(const struct device *dev)
{
	struct smhc_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = smhc_reset_ctrl(dev);
	k_mutex_unlock(&data->lock);

	return ret;
}

static int smhc_set_io(const struct device *dev, struct sdhc_io *ios)
{
	const struct smhc_config *cfg = dev->config;
	struct smhc_data *data = dev->data;
	bool ddr = ios->timing == SDHC_TIMING_DDR52 || ios->timing == SDHC_TIMING_DDR50;
	int ret = 0;

	if (ios->signal_voltage == SD_VOL_1_8_V || ios->signal_voltage == SD_VOL_1_2_V) {
		return -ENOTSUP;
	}
	if (ios->clock != 0U && (ios->clock < cfg->f_min || ios->clock > cfg->f_max)) {
		LOG_ERR("unsupported clock %u", ios->clock);
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	if (ios->power_mode != data->ios.power_mode && ios->power_mode != 0) {
		if (cfg->pwr.port != NULL) {
			gpio_pin_set_dt(&cfg->pwr, ios->power_mode == SDHC_POWER_ON);
		}
		if (ios->power_mode == SDHC_POWER_OFF) {
			ios->clock = 0;
		}
	}

	if (ios->bus_width != data->ios.bus_width && ios->bus_width != 0) {
		switch (ios->bus_width) {
		case SDHC_BUS_WIDTH1BIT:
			smhc_wr(dev, SMHC_WIDTH, 0);
			break;
		case SDHC_BUS_WIDTH4BIT:
			smhc_wr(dev, SMHC_WIDTH, 1);
			break;
		case SDHC_BUS_WIDTH8BIT:
			smhc_wr(dev, SMHC_WIDTH, 2);
			break;
		default:
			ret = -EINVAL;
			goto out;
		}
	}

	if (ios->clock != data->ios.clock || ios->timing != data->ios.timing) {
		ret = smhc_set_clock(dev, ios->clock, ddr);
		if (ret) {
			LOG_ERR("clock update failed: %d", ret);
			goto out;
		}
	}

	data->ios = *ios;
out:
	k_mutex_unlock(&data->lock);
	return ret;
}

static int smhc_get_card_present(const struct device *dev)
{
	const struct smhc_config *cfg = dev->config;

	if (cfg->non_removable || cfg->cd.port == NULL) {
		return 1;
	}

	return gpio_pin_get_dt(&cfg->cd) > 0 ? 1 : 0;
}

static int smhc_card_busy(const struct device *dev)
{
	return (smhc_rd(dev, SMHC_STAS) & STAS_CARD_BUSY) ? 1 : 0;
}

static int smhc_execute_tuning(const struct device *dev)
{
	return -ENOTSUP;
}

static int smhc_get_host_props(const struct device *dev, struct sdhc_host_props *props)
{
	const struct smhc_config *cfg = dev->config;

	memset(props, 0, sizeof(*props));
	props->f_min = cfg->f_min;
	props->f_max = cfg->f_max;
	props->power_delay = cfg->power_delay;
	props->is_spi = false;
	props->host_caps.vol_330_support = 1;
	props->host_caps.high_spd_support = 1;
	props->host_caps.bus_4_bit_support = cfg->bus_width >= 4;
	props->host_caps.bus_8_bit_support = cfg->bus_width >= 8;
	props->host_caps.sdio_async_interrupt_support = 1;
	props->max_current_330 = 200;

	return 0;
}

static void smhc_cd_isr(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	struct smhc_data *data = CONTAINER_OF(cb, struct smhc_data, cd_cb);
	int present = smhc_get_card_present(data->dev);

	if (data->cb != NULL) {
		int src = present ? SDHC_INT_INSERTED : SDHC_INT_REMOVED;

		if (data->cb_sources & src) {
			data->cb(data->dev, src, data->cb_data);
		}
	}
}

static int smhc_enable_interrupt(const struct device *dev, sdhc_interrupt_cb_t callback,
				 int sources, void *user_data)
{
	const struct smhc_config *cfg = dev->config;
	struct smhc_data *data = dev->data;
	int ret = 0;

	k_mutex_lock(&data->lock, K_FOREVER);
	data->cb = callback;
	data->cb_data = user_data;
	data->cb_sources |= sources;

	if (sources & SDHC_INT_SDIO) {
		unsigned int key = irq_lock();

		data->sdio_mask = INT_SDIO;
		smhc_wr(dev, SMHC_IMASK, smhc_rd(dev, SMHC_IMASK) | INT_SDIO);
		irq_unlock(key);
	}

	if ((sources & (SDHC_INT_INSERTED | SDHC_INT_REMOVED)) && cfg->cd.port != NULL) {
		ret = gpio_pin_interrupt_configure_dt(&cfg->cd, GPIO_INT_EDGE_BOTH);
	}
	k_mutex_unlock(&data->lock);

	return ret;
}

static int smhc_disable_interrupt(const struct device *dev, int sources)
{
	const struct smhc_config *cfg = dev->config;
	struct smhc_data *data = dev->data;
	int ret = 0;

	k_mutex_lock(&data->lock, K_FOREVER);
	data->cb_sources &= ~sources;

	if (sources & SDHC_INT_SDIO) {
		unsigned int key = irq_lock();

		data->sdio_mask = 0;
		smhc_wr(dev, SMHC_IMASK, smhc_rd(dev, SMHC_IMASK) & ~INT_SDIO);
		irq_unlock(key);
	}

	if ((sources & (SDHC_INT_INSERTED | SDHC_INT_REMOVED)) && cfg->cd.port != NULL &&
	    !(data->cb_sources & (SDHC_INT_INSERTED | SDHC_INT_REMOVED))) {
		ret = gpio_pin_interrupt_configure_dt(&cfg->cd, GPIO_INT_DISABLE);
	}
	k_mutex_unlock(&data->lock);

	return ret;
}

static int smhc_init(const struct device *dev)
{
	const struct smhc_config *cfg = dev->config;
	struct smhc_data *data = dev->data;
	int ret;

	data->dev = dev;
	k_mutex_init(&data->lock);
	k_sem_init(&data->done, 0, 1);

	ret = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
	if (ret) {
		return ret;
	}

	if (cfg->cd.port != NULL) {
		if (!gpio_is_ready_dt(&cfg->cd)) {
			return -ENODEV;
		}
		ret = gpio_pin_configure_dt(&cfg->cd, GPIO_INPUT);
		if (ret) {
			return ret;
		}
		gpio_init_callback(&data->cd_cb, smhc_cd_isr, BIT(cfg->cd.pin));
		ret = gpio_add_callback(cfg->cd.port, &data->cd_cb);
		if (ret) {
			return ret;
		}
	}

	if (cfg->pwr.port != NULL) {
		if (!gpio_is_ready_dt(&cfg->pwr)) {
			return -ENODEV;
		}
		ret = gpio_pin_configure_dt(&cfg->pwr, GPIO_OUTPUT_INACTIVE);
		if (ret) {
			return ret;
		}
	}

	ret = clock_control_on(cfg->clock_dev, cfg->bus_clk);
	if (ret) {
		return ret;
	}
	ret = reset_line_toggle_dt(&cfg->reset);
	if (ret) {
		return ret;
	}

	/* module clock from the oscillator until the first set_io() */
	sys_write32(MOD_CLK_GATE, CCU_BASE + cfg->clock_reg);

	ret = smhc_reset_ctrl(dev);
	if (ret) {
		LOG_ERR("controller reset failed");
		return ret;
	}
	smhc_wr(dev, SMHC_WIDTH, 0);

	cfg->irq_config();

	return 0;
}

static const struct sdhc_driver_api smhc_api = {
	.reset = smhc_hw_reset,
	.request = smhc_request,
	.set_io = smhc_set_io,
	.get_card_present = smhc_get_card_present,
	.execute_tuning = smhc_execute_tuning,
	.card_busy = smhc_card_busy,
	.get_host_props = smhc_get_host_props,
	.enable_interrupt = smhc_enable_interrupt,
	.disable_interrupt = smhc_disable_interrupt,
};

#define SMHC_INIT(n)								\
	PINCTRL_DT_INST_DEFINE(n);						\
	static void smhc_irq_config_##n(void)					\
	{									\
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), smhc_isr,	\
			    DEVICE_DT_INST_GET(n), 0);				\
		irq_enable(DT_INST_IRQN(n));					\
	}									\
	static const struct smhc_config smhc_config_##n = {			\
		.base = DT_INST_REG_ADDR(n),					\
		.clock_reg = DT_INST_PROP(n, clock_reg),			\
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR_BY_NAME(n, bus)),	\
		.bus_clk = (clock_control_subsys_t)				\
			DT_INST_CLOCKS_CELL_BY_NAME(n, bus, clkid),		\
		.reset = RESET_DT_SPEC_INST_GET(n),				\
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),			\
		.cd = GPIO_DT_SPEC_INST_GET_OR(n, cd_gpios, {0}),		\
		.pwr = GPIO_DT_SPEC_INST_GET_OR(n, pwr_gpios, {0}),		\
		.irq_config = smhc_irq_config_##n,				\
		.f_min = DT_INST_PROP(n, min_bus_freq),			\
		.f_max = DT_INST_PROP(n, max_bus_freq),			\
		.power_delay = DT_INST_PROP(n, power_delay_ms),		\
		.bus_width = DT_INST_PROP(n, bus_width),			\
		.non_removable = DT_INST_PROP(n, non_removable),		\
	};									\
	static struct smhc_data smhc_data_##n;					\
	DEVICE_DT_INST_DEFINE(n, smhc_init, NULL, &smhc_data_##n, &smhc_config_##n,	\
			      POST_KERNEL, CONFIG_SDHC_INIT_PRIORITY, &smhc_api);

DT_INST_FOREACH_STATUS_OKAY(SMHC_INIT)

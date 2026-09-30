/* SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later */
/*
 * Display pipeline framework - MIPI DSI host/device abstraction.
 */
#ifndef __DPY_DSI_H__
#define __DPY_DSI_H__

#include <dpy/dpy_kms.h>

/* MIPI DSI processor-to-peripheral data types */
#define DPY_DSI_V_SYNC_START			0x01
#define DPY_DSI_V_SYNC_END			0x11
#define DPY_DSI_H_SYNC_START			0x21
#define DPY_DSI_H_SYNC_END			0x31
#define DPY_DSI_EOT_PACKET			0x08
#define DPY_DSI_GENERIC_SHORT_WRITE_0		0x03
#define DPY_DSI_GENERIC_SHORT_WRITE_1		0x13
#define DPY_DSI_GENERIC_SHORT_WRITE_2		0x23
#define DPY_DSI_GENERIC_READ_0			0x04
#define DPY_DSI_GENERIC_READ_1			0x14
#define DPY_DSI_GENERIC_READ_2			0x24
#define DPY_DSI_DCS_SHORT_WRITE			0x05
#define DPY_DSI_DCS_SHORT_WRITE_PARAM		0x15
#define DPY_DSI_DCS_READ			0x06
#define DPY_DSI_SET_MAX_RETURN_PACKET_SIZE	0x37
#define DPY_DSI_NULL_PACKET			0x09
#define DPY_DSI_BLANKING_PACKET			0x19
#define DPY_DSI_GENERIC_LONG_WRITE		0x29
#define DPY_DSI_DCS_LONG_WRITE			0x39
#define DPY_DSI_PACKED_PIXEL_STREAM_16		0x0e
#define DPY_DSI_PACKED_PIXEL_STREAM_18		0x1e
#define DPY_DSI_PIXEL_STREAM_3BYTE_18		0x2e
#define DPY_DSI_PACKED_PIXEL_STREAM_24		0x3e

/* peripheral-to-processor data types */
#define DPY_DSI_RX_ACK_ERR_REPORT		0x02
#define DPY_DSI_RX_EOT				0x08
#define DPY_DSI_RX_GENERIC_SHORT_1		0x11
#define DPY_DSI_RX_GENERIC_SHORT_2		0x12
#define DPY_DSI_RX_GENERIC_LONG			0x1a
#define DPY_DSI_RX_DCS_LONG			0x1c
#define DPY_DSI_RX_DCS_SHORT_1			0x21
#define DPY_DSI_RX_DCS_SHORT_2			0x22

/* common DCS commands */
#define DPY_DCS_NOP				0x00
#define DPY_DCS_SOFT_RESET			0x01
#define DPY_DCS_GET_POWER_MODE			0x0a
#define DPY_DCS_ENTER_SLEEP_MODE		0x10
#define DPY_DCS_EXIT_SLEEP_MODE			0x11
#define DPY_DCS_SET_DISPLAY_OFF			0x28
#define DPY_DCS_SET_DISPLAY_ON			0x29
#define DPY_DCS_WRITE_MEMORY_START		0x2c
#define DPY_DCS_WRITE_MEMORY_CONTINUE		0x3c
#define DPY_DCS_SET_TEAR_ON			0x35
#define DPY_DCS_SET_PIXEL_FORMAT		0x3a

enum dpy_dsi_format {
	DPY_DSI_FMT_RGB888 = 0,
	DPY_DSI_FMT_RGB666,		/* 18 bit, loosely packed */
	DPY_DSI_FMT_RGB666_PACKED,
	DPY_DSI_FMT_RGB565,
};

#define DPY_DSI_MODE_VIDEO		(1U << 0)
#define DPY_DSI_MODE_VIDEO_BURST	(1U << 1)
#define DPY_DSI_MODE_VIDEO_SYNC_PULSE	(1U << 2)
#define DPY_DSI_MODE_LPM		(1U << 3)	/* commands in LP */
#define DPY_DSI_CLOCK_NON_CONTINUOUS	(1U << 4)
#define DPY_DSI_MODE_NO_EOT_PACKET	(1U << 5)

#define DPY_DSI_MSG_REQ_ACK		(1U << 0)
#define DPY_DSI_MSG_USE_LPM		(1U << 1)

struct dpy_dsi_msg {
	uint8_t channel;
	uint8_t type;
	uint16_t flags;
	size_t tx_len;
	const void *tx_buf;
	size_t rx_len;
	void *rx_buf;
};

struct dpy_dsi_host;
struct dpy_dsi_device;

struct dpy_dsi_host_ops {
	int (*attach)(struct dpy_dsi_host *host, struct dpy_dsi_device *dev);
	int (*detach)(struct dpy_dsi_host *host, struct dpy_dsi_device *dev);
	/* returns bytes received for reads, 0 for writes, <0 on error */
	int (*transfer)(struct dpy_dsi_host *host,
			const struct dpy_dsi_msg *msg);
};

struct dpy_dsi_host {
	struct dpy_list head;
	const struct dpy_gnode *node;
	const struct dpy_dsi_host_ops *ops;
	void *priv;
	struct dpy_dsi_device *device;
};

struct dpy_dsi_device {
	struct dpy_dsi_host *host;
	const struct dpy_gnode *node;
	uint8_t channel;
	uint8_t lanes;
	uint8_t format;			/* enum dpy_dsi_format */
	uint32_t mode_flags;		/* DPY_DSI_MODE_* */
	/* D-PHY timing tweaks (0 = driver default) */
	uint8_t hs_trail;
	uint8_t clk_trail;
};

int dpy_dsi_host_register(struct dpy_dsi_host *host);
void dpy_dsi_host_unregister(struct dpy_dsi_host *host);
struct dpy_dsi_host *dpy_dsi_host_find(const struct dpy_gnode *node);

int dpy_dsi_attach(struct dpy_dsi_device *dev, struct dpy_dsi_host *host);
int dpy_dsi_detach(struct dpy_dsi_device *dev);

uint32_t dpy_dsi_format_bpp(uint32_t format);

int dpy_dsi_dcs_write(struct dpy_dsi_device *dev, uint8_t cmd,
		      const uint8_t *data, size_t len);
int dpy_dsi_generic_write(struct dpy_dsi_device *dev, const uint8_t *data,
			  size_t len);
/* returns the number of bytes read */
int dpy_dsi_dcs_read(struct dpy_dsi_device *dev, uint8_t cmd, uint8_t *data,
		     size_t len);
int dpy_dsi_set_max_return_packet_size(struct dpy_dsi_device *dev,
				       uint16_t size);

/* packet helpers shared by DSI host drivers */
uint8_t dpy_dsi_ecc(uint32_t header24);
uint16_t dpy_dsi_crc(const uint8_t *data, size_t len);
uint16_t dpy_dsi_crc_repeat(uint8_t byte, size_t len);

#endif /* __DPY_DSI_H__ */

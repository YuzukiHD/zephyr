// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Display pipeline framework - MIPI DSI host registry and packet helpers.
 */
#define DPY_LOG_TAG "dsi"
#include <dpy/dpy_dsi.h>
#include <dpy/dpy_log.h>

static struct dpy_list dpy_dsi_hosts = DPY_LIST_INIT(dpy_dsi_hosts);

int dpy_dsi_host_register(struct dpy_dsi_host *host)
{
	if (!host->ops || !host->ops->transfer)
		return -EINVAL;
	dpy_list_add_tail(&host->head, &dpy_dsi_hosts);
	return 0;
}

void dpy_dsi_host_unregister(struct dpy_dsi_host *host)
{
	dpy_list_del(&host->head);
}

struct dpy_dsi_host *dpy_dsi_host_find(const struct dpy_gnode *node)
{
	struct dpy_dsi_host *host;

	dpy_list_for_each_entry(host, &dpy_dsi_hosts, head)
		if (host->node == node)
			return host;
	return NULL;
}

int dpy_dsi_attach(struct dpy_dsi_device *dev, struct dpy_dsi_host *host)
{
	int ret = 0;

	if (host->device)
		return -EBUSY;
	if (host->ops->attach)
		ret = host->ops->attach(host, dev);
	if (ret)
		return ret;
	host->device = dev;
	dev->host = host;
	return 0;
}

int dpy_dsi_detach(struct dpy_dsi_device *dev)
{
	struct dpy_dsi_host *host = dev->host;

	if (!host)
		return 0;
	if (host->ops->detach)
		host->ops->detach(host, dev);
	host->device = NULL;
	dev->host = NULL;
	return 0;
}

uint32_t dpy_dsi_format_bpp(uint32_t format)
{
	switch (format) {
	case DPY_DSI_FMT_RGB666_PACKED:
		return 18;
	case DPY_DSI_FMT_RGB565:
		return 16;
	case DPY_DSI_FMT_RGB666:
	case DPY_DSI_FMT_RGB888:
	default:
		return 24;
	}
}

static int dpy_dsi_transfer(struct dpy_dsi_device *dev,
			    struct dpy_dsi_msg *msg)
{
	if (!dev->host)
		return -ENODEV;
	msg->channel = dev->channel;
	if (dev->mode_flags & DPY_DSI_MODE_LPM)
		msg->flags |= DPY_DSI_MSG_USE_LPM;
	return dev->host->ops->transfer(dev->host, msg);
}

int dpy_dsi_dcs_write(struct dpy_dsi_device *dev, uint8_t cmd,
		      const uint8_t *data, size_t len)
{
	uint8_t buf[64];
	struct dpy_dsi_msg msg;

	if (len + 1 > sizeof(buf))
		return -EINVAL;
	buf[0] = cmd;
	if (len)
		memcpy(&buf[1], data, len);

	memset(&msg, 0, sizeof(msg));
	msg.tx_buf = buf;
	msg.tx_len = len + 1;
	if (len == 0)
		msg.type = DPY_DSI_DCS_SHORT_WRITE;
	else if (len == 1)
		msg.type = DPY_DSI_DCS_SHORT_WRITE_PARAM;
	else
		msg.type = DPY_DSI_DCS_LONG_WRITE;
	return dpy_dsi_transfer(dev, &msg);
}

int dpy_dsi_generic_write(struct dpy_dsi_device *dev, const uint8_t *data,
			  size_t len)
{
	struct dpy_dsi_msg msg;

	memset(&msg, 0, sizeof(msg));
	msg.tx_buf = data;
	msg.tx_len = len;
	if (len == 0)
		msg.type = DPY_DSI_GENERIC_SHORT_WRITE_0;
	else if (len == 1)
		msg.type = DPY_DSI_GENERIC_SHORT_WRITE_1;
	else if (len == 2)
		msg.type = DPY_DSI_GENERIC_SHORT_WRITE_2;
	else
		msg.type = DPY_DSI_GENERIC_LONG_WRITE;
	return dpy_dsi_transfer(dev, &msg);
}

int dpy_dsi_dcs_read(struct dpy_dsi_device *dev, uint8_t cmd, uint8_t *data,
		     size_t len)
{
	struct dpy_dsi_msg msg;

	memset(&msg, 0, sizeof(msg));
	msg.type = DPY_DSI_DCS_READ;
	msg.tx_buf = &cmd;
	msg.tx_len = 1;
	msg.rx_buf = data;
	msg.rx_len = len;
	return dpy_dsi_transfer(dev, &msg);
}

int dpy_dsi_set_max_return_packet_size(struct dpy_dsi_device *dev,
				       uint16_t size)
{
	uint8_t buf[2] = { (uint8_t)(size & 0xff), (uint8_t)(size >> 8) };
	struct dpy_dsi_msg msg;

	memset(&msg, 0, sizeof(msg));
	msg.type = DPY_DSI_SET_MAX_RETURN_PACKET_SIZE;
	msg.tx_buf = buf;
	msg.tx_len = sizeof(buf);
	return dpy_dsi_transfer(dev, &msg);
}

/*
 * Packet header ECC: Hamming(24,30) code defined by the MIPI DSI spec.
 * Each parity bit is the XOR of the header bits selected by its mask.
 */
uint8_t dpy_dsi_ecc(uint32_t header24)
{
	static const uint32_t ecc_masks[6] = {
		0xf12cb7, 0xf2555b, 0x749a6d, 0xb8e38e, 0xdf03f0, 0xeffc00,
	};
	uint8_t ecc = 0;
	unsigned int i;

	header24 &= 0xffffff;
	for (i = 0; i < 6; i++)
		if (__builtin_parity(header24 & ecc_masks[i]))
			ecc |= (uint8_t)(1U << i);
	return ecc;
}

/* CRC-16/CCITT, reflected polynomial 0x8408, seed 0xffff */
static uint16_t dpy_dsi_crc_byte(uint16_t crc, uint8_t byte)
{
	unsigned int bit;

	for (bit = 0; bit < 8; bit++) {
		if ((crc ^ byte) & 1)
			crc = (uint16_t)((crc >> 1) ^ 0x8408);
		else
			crc >>= 1;
		byte >>= 1;
	}
	return crc;
}

uint16_t dpy_dsi_crc(const uint8_t *data, size_t len)
{
	uint16_t crc = 0xffff;

	while (len--)
		crc = dpy_dsi_crc_byte(crc, *data++);
	return crc;
}

uint16_t dpy_dsi_crc_repeat(uint8_t byte, size_t len)
{
	uint16_t crc = 0xffff;

	while (len--)
		crc = dpy_dsi_crc_byte(crc, byte);
	return crc;
}

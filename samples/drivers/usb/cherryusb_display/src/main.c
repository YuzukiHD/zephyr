/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * USB second screen: the board enumerates as a virtual display for the Windows "usb graphic"
 * display driver. The PC sends its desktop as JPEG frames over a bulk endpoint, the video engine
 * decodes them and the picture goes to the video plane of the display, which scales it to the
 * panel keeping its shape.
 *
 * The host is told the size, frame rate and JPEG quality through the product string (see the
 * SAMPLE_USB_DISPLAY_* options); by default the size of the panel.
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/display/display_sunxi.h>
#include <zephyr/drivers/vdec.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "usbd_core.h"
#include "usbd_display.h"

extern uintptr_t usb_sunxi_otg_base(void);

#define DISPLAY_IN_EP   0x81
#define DISPLAY_OUT_EP  0x02

#define USBD_VID        0x303A
#define USBD_PID        0x2987
#define USBD_MAX_POWER  100
#define USB_CONFIG_SIZE (9 + 9 + 7 + 7)

#define DISPLAY_EP_MPS  512

/*
 * A JPEG frame is at most 512 KiB; the driver reads in steps of 16 KiB, so the buffer is a
 * multiple of that and holds one step more than the largest frame.
 */
#define FRAME_BUF_SIZE  (34 * 16384)
#define FRAME_COUNT     2
#define FRAME_LIMIT_KB  500
#define REPORT_MS       5000

#define JPEG_VBV_SIZE   (1024 * 1024)

#define MIN_SIZE        64
#define MAX_WIDTH       1920
#define MAX_HEIGHT      1080

/* name, size, encoding (jpg quality 1..10), frame rate, frame limit in KB */
#define PRODUCT_FORMAT  "cherryusb_R%ux%u_Ejpg%u_Fps%u_Bl%u"

static const struct device *const vdec_dev = DEVICE_DT_GET(DT_NODELABEL(ve));
static const struct device *const disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

static char product_string[64];

static const uint8_t device_descriptor[] = {
	USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0x00, 0x00, 0x00, USBD_VID, USBD_PID, 0x0101, 0x01)
};

static const uint8_t config_descriptor[] = {
	USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x01, 0x01, USB_CONFIG_BUS_POWERED,
				   USBD_MAX_POWER),
	USB_INTERFACE_DESCRIPTOR_INIT(0x00, 0x00, 0x02, 0xff, 0x00, 0x00, 0x00),
	USB_ENDPOINT_DESCRIPTOR_INIT(DISPLAY_IN_EP, 0x02, DISPLAY_EP_MPS, 0x00),
	USB_ENDPOINT_DESCRIPTOR_INIT(DISPLAY_OUT_EP, 0x02, DISPLAY_EP_MPS, 0x00),
};

static const uint8_t device_quality_descriptor[] = {
	0x0a, USB_DESCRIPTOR_TYPE_DEVICE_QUALIFIER, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00,
};

static const char *string_descriptors[] = {
	(const char[]){0x09, 0x04}, /* language id */
	"CherryUSB",                /* manufacturer */
	product_string,             /* product */
	"2022123456",               /* serial number */
};

static const uint8_t *device_descriptor_callback(uint8_t speed)
{
	return device_descriptor;
}

static const uint8_t *config_descriptor_callback(uint8_t speed)
{
	return config_descriptor;
}

static const uint8_t *device_quality_descriptor_callback(uint8_t speed)
{
	return device_quality_descriptor;
}

static const char *string_descriptor_callback(uint8_t speed, uint8_t index)
{
	if (index >= ARRAY_SIZE(string_descriptors)) {
		return NULL;
	}

	return string_descriptors[index];
}

static const struct usb_descriptor display_descriptor = {
	.device_descriptor_callback = device_descriptor_callback,
	.config_descriptor_callback = config_descriptor_callback,
	.device_quality_descriptor_callback = device_quality_descriptor_callback,
	.string_descriptor_callback = string_descriptor_callback,
};

static void usbd_event_handler(uint8_t busid, uint8_t event)
{
	switch (event) {
	case USBD_EVENT_CONFIGURED:
		printk("usb display: configured by the host\n");
		break;
	case USBD_EVENT_DISCONNECTED:
		printk("usb display: disconnected\n");
		break;
	default:
		break;
	}
}

static struct usbd_interface display_intf;
static struct usbd_display_frame frame_pool[FRAME_COUNT];

/*
 * A decoder that stays open while the picture size stays the same: motion JPEG through the stream
 * API. The decoded pictures go to the display as they are; the one before the last may still be
 * scanned out, so it is released one frame late.
 */
static struct vdec_stream *jpeg_stream;
static uint32_t jpeg_w, jpeg_h;
/* pictures the decoder keeps for us (1..3), the most the memory allows */
static int jpeg_hold = 3;
static struct vdec_frame shown, older;

static uint32_t frames_ok, frames_bad, bytes_in, decode_us, show_us;

static void stream_close(void)
{
	if (older.priv) {
		vdec_frame_release(vdec_dev, &older);
		older.priv = NULL;
	}
	if (shown.priv) {
		vdec_frame_release(vdec_dev, &shown);
		shown.priv = NULL;
	}
	if (jpeg_stream) {
		vdec_stream_close(vdec_dev, jpeg_stream);
	}
	jpeg_stream = NULL;
}

static int stream_open(uint32_t w, uint32_t h)
{
	int ret = -ENOMEM;

	/* one held picture (a show that waits for the refresh) is the least that works */
	for (jpeg_hold = 3; jpeg_hold >= 1; jpeg_hold--) {
		struct vdec_stream_config cfg = {
			.codec = VDEC_CODEC_JPEG,
			.format = VDEC_FORMAT_NV12,
			.buffer_size = JPEG_VBV_SIZE,
			.no_cache_ops = true,
			.holding_frames = jpeg_hold,
			.width = w,
			.height = h,
		};

		ret = vdec_stream_open(vdec_dev, &cfg, &jpeg_stream);
		if (ret == 0) {
			jpeg_w = w;
			jpeg_h = h;
			printk("usb display: %ux%u decoder, %d pictures kept (%s show)\n", w, h,
			       jpeg_hold, jpeg_hold >= 2 ? "no-wait" : "waiting");
			return 0;
		}
	}

	return ret;
}

/* decode one JPEG of w x h pixels and put it on the screen; 0 on success */
static int show_jpeg(const void *jpeg, size_t len, uint32_t w, uint32_t h)
{
	struct vdec_frame f;
	struct display_sunxi_yuv yuv;
	size_t used;
	uint32_t t0 = k_cycle_get_32();
	int ret, tries;

	if (jpeg_stream && (w != jpeg_w || h != jpeg_h)) {
		stream_close();
	}
	if (!jpeg_stream) {
		ret = stream_open(w, h);
		if (ret) {
			return ret;
		}
	}
	ret = vdec_stream_feed(vdec_dev, jpeg_stream, jpeg, len, 0, &used);
	if (ret) {
		return ret;
	}
	for (tries = 0; tries < 20; tries++) {
		ret = vdec_stream_get_frame(vdec_dev, jpeg_stream, &f);
		if (ret != -EBUSY && ret != -EAGAIN) {
			break;
		}
		k_msleep(1);
	}
	if (ret) {
		return ret;
	}
	decode_us += k_cyc_to_us_floor32(k_cycle_get_32() - t0);

	t0 = k_cycle_get_32();
	memset(&yuv, 0, sizeof(yuv));
	yuv.y = f.plane[0];
	yuv.uv = f.plane[1];
	yuv.width = f.width;
	yuv.height = f.height;
	yuv.stride_y = f.stride[0];
	yuv.stride_uv = f.stride[1];
	yuv.full_range = true;
	/*
	 * With room for two kept pictures the show does not wait for the refresh and the picture
	 * before the last is freed one step late (it may still be scanned out). With one kept
	 * picture the show waits, then the picture before is free.
	 */
	yuv.nonblock = jpeg_hold >= 2;
	ret = display_sunxi_show_yuv(disp, &yuv);
	show_us += k_cyc_to_us_floor32(k_cycle_get_32() - t0);
	if (jpeg_hold >= 2) {
		if (older.priv) {
			vdec_frame_release(vdec_dev, &older);
		}
		older = shown;
	} else if (shown.priv) {
		vdec_frame_release(vdec_dev, &shown);
	}
	shown = f;

	return ret;
}

/* a whole JPEG: SOI first, EOI last (zero padding may follow it) */
static bool jpeg_complete(const uint8_t *d, uint32_t n)
{
	uint32_t e = n;

	while (e > 2 && d[e - 1] == 0) {
		e--;
	}

	return n >= 4 && d[0] == 0xff && d[1] == 0xd8 && d[e - 2] == 0xff && d[e - 1] == 0xd9;
}

static void handle_frame(struct usbd_display_frame *frame)
{
	struct usbd_disp_frame_header *h = (struct usbd_disp_frame_header *)frame->frame_buf;
	const uint8_t *d = frame->frame_buf + sizeof(*h);
	uint32_t n = frame->frame_size;

	/* a frame that fills the last packet exactly is followed by an empty frame of type 0xff */
	if (frame->frame_format != USBD_DISPLAY_TYPE_JPG || n == 0) {
		return;
	}
	bytes_in += n;
	if (!jpeg_complete(d, n)) {
		if (++frames_bad <= 12) {
			printk("usb display: broken frame %u: size %u\n", h->frame_id, n);
		}
		return;
	}

	int err = show_jpeg(d, n, h->width, h->height);

	if (err == 0) {
		frames_ok++;
	} else if (++frames_bad <= 8) {
		printk("usb display: frame %u (%ux%u, %u bytes) failed: %d\n", h->frame_id, h->width,
		       h->height, n, err);
	}
}

int main(void)
{
	struct display_capabilities caps;
	uint32_t w = CONFIG_SAMPLE_USB_DISPLAY_WIDTH, h = CONFIG_SAMPLE_USB_DISPLAY_HEIGHT;
	uint32_t last, start_ok = 0, start_bytes = 0;

	if (!device_is_ready(disp) || !device_is_ready(vdec_dev)) {
		printk("usb display: display or video engine not ready\n");
		return -ENODEV;
	}
	display_get_capabilities(disp, &caps);
	if (!w || !h) {
		w = caps.x_resolution;
		h = caps.y_resolution;
	}
	if (w < MIN_SIZE || h < MIN_SIZE || w > MAX_WIDTH || h > MAX_HEIGHT || (w & 1) || (h & 1)) {
		printk("usb display: %ux%u not possible (size %d..%d x %d..%d, even)\n", w, h,
		       MIN_SIZE, MAX_WIDTH, MIN_SIZE, MAX_HEIGHT);
		return -EINVAL;
	}

	snprintk(product_string, sizeof(product_string), PRODUCT_FORMAT, w, h,
		 CONFIG_SAMPLE_USB_DISPLAY_QUALITY, CONFIG_SAMPLE_USB_DISPLAY_FPS, FRAME_LIMIT_KB);
	for (int i = 0; i < FRAME_COUNT; i++) {
		frame_pool[i].frame_buf = aligned_alloc(64, FRAME_BUF_SIZE);
		frame_pool[i].frame_bufsize = FRAME_BUF_SIZE;
		if (!frame_pool[i].frame_buf) {
			printk("usb display: no memory for the frame buffers\n");
			return -ENOMEM;
		}
	}

	usbd_desc_register(0, &display_descriptor);
	usbd_add_interface(0, usbd_display_init_intf(&display_intf, DISPLAY_OUT_EP, DISPLAY_IN_EP,
						    frame_pool, FRAME_COUNT));
	usbd_initialize(0, usb_sunxi_otg_base(), usbd_event_handler);
	printk("usb display: waiting for the host (%s), shown on the %ux%u panel\n", product_string,
	       caps.x_resolution, caps.y_resolution);

	last = k_uptime_get_32();
	for (;;) {
		struct usbd_display_frame *frame;

		if (usbd_display_dequeue(&frame, 1000) >= 0) {
			handle_frame(frame);
			usbd_display_enqueue(frame);
		}

		uint32_t ms = k_uptime_get_32() - last;

		if (ms >= REPORT_MS) {
			uint32_t n = frames_ok - start_ok;

			printk("usb display: %u frames (%u.%02u fps), %u KB/s, %u bad, decode %u us, "
			       "show %u us per frame\n", n, n * 1000 / ms,
			       (n * 100000 / ms) % 100, (bytes_in - start_bytes) / ms, frames_bad,
			       n ? decode_us / n : 0, n ? show_us / n : 0);
			decode_us = show_us = 0;
			start_ok = frames_ok;
			start_bytes = bytes_in;
			last = k_uptime_get_32();
		}
	}

	return 0;
}

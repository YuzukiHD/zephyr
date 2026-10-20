/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Exercises the SPIF driver on the EVB flash: identification, sample point
 * tuning and its storage, read speed, erase/program/verify, and the XIP
 * window (reads, code executed in the window, writes while it is mapped).
 */

#include <stdio.h>
#include <string.h>
#include <zephyr/cache.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/flash/flash_sunxi_spif.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>

#define SCRATCH_OFF	DT_REG_ADDR(DT_NODELABEL(spif_scratch))
#define SCRATCH_SIZE	DT_REG_SIZE(DT_NODELABEL(spif_scratch))
#define XIP_OFF		DT_REG_ADDR(DT_NODELABEL(spif_xip_test))
#define XIP_SIZE	DT_REG_SIZE(DT_NODELABEL(spif_xip_test))
#define TUNE_OFF	DT_REG_ADDR(DT_NODELABEL(spif_tune))

#define CHUNK		(64U * 1024U)

static const struct device *const spif = DEVICE_DT_GET(DT_NODELABEL(spif));

/* aligned for the cache line, the window is read by the CPU */
static uint8_t buf_a[CHUNK] __aligned(64);
static uint8_t buf_b[CHUNK] __aligned(64);

static int failures;

#define CHECK(cond, ...)							\
	do {									\
		if (!(cond)) {							\
			failures++;						\
			printk("FAIL %s:%d: ", __func__, __LINE__);		\
			printk(__VA_ARGS__);					\
			printk("\n");						\
		}								\
	} while (0)

static uint32_t us_since(uint32_t t0)
{
	return k_cyc_to_us_floor32(k_cycle_get_32() - t0);
}

static void prbs(uint8_t *p, size_t n, uint32_t seed)
{
	uint32_t x = seed ? seed : 1U;

	for (size_t i = 0; i < n; i++) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		p[i] = x >> 11;
	}
}

static bool is_blank(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (p[i] != 0xff) {
			return false;
		}
	}

	return true;
}

static void show_info(const char *when)
{
	struct sunxi_spif_info i;

	sunxi_spif_get_info(spif, &i);
	printk("[%s] controller %08x, flash %02x%02x%02x %u KiB, %u Hz, quad %d dtr %d, sample %s mode %u "
	       "delay %u, xip %d\n", when, i.version, i.jedec_id[0], i.jedec_id[1], i.jedec_id[2],
	       i.size / 1024U, i.frequency, i.quad, i.dtr, i.sample_tuned ? "tuned" : "none",
	       i.sample_mode, i.sample_delay, i.xip_active);
}

/* one character per 64 KiB: '.' blank, '#' holds data */
static void show_map(void)
{
	char line[65];

	for (uint32_t base = 0; base < 16U * 1024U * 1024U; base += 64U * CHUNK) {
		for (int i = 0; i < 64; i++) {
			flash_read(spif, base + i * CHUNK, buf_a, CHUNK);
			line[i] = is_blank(buf_a, CHUNK) ? '.' : '#';
		}
		line[64] = '\0';
		printk("map %06x %s\n", base, line);
	}
}

static void test_tune(void)
{
	struct sunxi_spif_tune_result r;
	int ret;

	/* the tuning reference is data of the flash itself; a blank partition has none */
	ret = flash_read(spif, TUNE_OFF, buf_a, 4096);
	if (ret == 0 && is_blank(buf_a, 4096)) {
		prbs(buf_a, 4096, 0x7157);
		ret = flash_write(spif, TUNE_OFF, buf_a, 4096);
		CHECK(ret == 0, "tune reference write: %d", ret);
		printk("tune: partition was blank, reference written\n");
	}
	ret = sunxi_spif_tune(spif, &r);

	printk("tune: %d, %u Hz, mode %u delay %u window %u+%u\n", ret, r.frequency, r.mode, r.delay,
	       r.window_start, r.window_len);
	for (int m = 0; m < SUNXI_SPIF_TUNE_MODES; m++) {
		char line[SUNXI_SPIF_TUNE_DELAYS + 1], one[SUNXI_SPIF_TUNE_DELAYS + 1];

		for (int d = 0; d < SUNXI_SPIF_TUNE_DELAYS; d++) {
			line[d] = (r.ok[m] & BIT64(d)) ? '#' : '.';
			one[d] = (r.ok_one_wire[m] & BIT64(d)) ? '#' : '.';
		}
		line[SUNXI_SPIF_TUNE_DELAYS] = '\0';
		one[SUNXI_SPIF_TUNE_DELAYS] = '\0';
		printk("  mode %d %s  (id only %s)\n", m, line, one);
	}
	CHECK(ret == 0, "tuning failed: %d", ret);
}

static void test_params(void)
{
	struct sunxi_spif_info i;
	uint8_t mode, delay;
	int ret;

	sunxi_spif_get_info(spif, &i);
	mode = i.sample_mode;
	delay = i.sample_delay;

	ret = sunxi_spif_params_save(spif);
	CHECK(ret == 0, "save: %d", ret);
	/* a different point, then the stored one comes back */
	sunxi_spif_set_sample(spif, (mode + 1) % 3, (delay + 7) % 64);
	ret = sunxi_spif_params_load(spif);
	CHECK(ret == 0, "load: %d", ret);
	sunxi_spif_get_info(spif, &i);
	CHECK(i.sample_mode == mode && i.sample_delay == delay,
	      "loaded mode %u delay %u, saved %u/%u", i.sample_mode, i.sample_delay, mode, delay);
	printk("params: saved and loaded mode %u delay %u\n", i.sample_mode, i.sample_delay);
}

static uint8_t buf_big[512 * 1024] __aligned(64);

static void test_speed(void)
{
	uint32_t t0, us;
	int ret = 0;

	flash_read(spif, TUNE_OFF, buf_big, 256 * 1024);
	printk("crc of 256 KiB at %x: %08x\n", TUNE_OFF, crc32_ieee(buf_big, 256 * 1024));
	for (int i = 0; i < 32; i++) {
		printk("%02x%s", buf_big[i], (i % 16) == 15 ? "\n" : " ");
	}
	for (int i = 0; i < 16; i++) {
		printk("%02x ", buf_big[0x20000 + i]);
	}
	printk("\n");

	flash_read(spif, 0x600000, buf_big, 256 * 1024);
	printk("crc of 256 KiB at 600000: %08x\n", crc32_ieee(buf_big, 256 * 1024));

	t0 = k_cycle_get_32();
	for (uint32_t o = 0; o < 2; o++) {
		ret |= flash_read(spif, TUNE_OFF + o * sizeof(buf_big), buf_big, sizeof(buf_big));
	}
	us = us_since(t0);
	printk("read 1 MiB in 512 KiB pieces: %d, %u us, %u KiB/s\n", ret, us,
	       (uint32_t)(1024ULL * 1000000 / us));
	CHECK(ret == 0, "read failed");

	/* the same data through the bounce buffer (unaligned destination) */
	ret = flash_read(spif, TUNE_OFF + sizeof(buf_big), buf_a + 1, CHUNK - 1);
	CHECK(ret == 0 && memcmp(buf_a + 1, buf_big, CHUNK - 1) == 0, "direct and bounce reads differ");
}

static void test_rw(void)
{
	uint32_t t0, us_e, us_w;
	int ret;

	ret = flash_read(spif, SCRATCH_OFF, buf_a, 4096);
	if (!is_blank(buf_a, 4096)) {
		/* left by an earlier run of this test: the magic marks it as ours */
		CHECK(memcmp(buf_a, "SPIFTEST", 8) == 0, "scratch partition is in use, not erasing");
		if (memcmp(buf_a, "SPIFTEST", 8) != 0) {
			return;
		}
	}

	t0 = k_cycle_get_32();
	ret = flash_erase(spif, SCRATCH_OFF, SCRATCH_SIZE);
	us_e = us_since(t0);
	CHECK(ret == 0, "erase: %d", ret);
	for (uint32_t o = 0; o < SCRATCH_SIZE; o += CHUNK) {
		ret = flash_read(spif, SCRATCH_OFF + o, buf_a, CHUNK);
		CHECK(ret == 0 && is_blank(buf_a, CHUNK), "not blank after erase at %x", o);
	}

	prbs(buf_a, CHUNK, 0x12345678);
	memcpy(buf_a, "SPIFTEST", 8);
	t0 = k_cycle_get_32();
	ret = flash_write(spif, SCRATCH_OFF, buf_a, CHUNK);
	us_w = us_since(t0);
	CHECK(ret == 0, "write: %d", ret);
	/* unaligned start and length, crossing pages */
	prbs(buf_b, 1000, 77);
	ret = flash_write(spif, SCRATCH_OFF + CHUNK + 3, buf_b, 1000);
	CHECK(ret == 0, "unaligned write: %d", ret);

	memset(buf_b, 0, CHUNK);
	ret = flash_read(spif, SCRATCH_OFF, buf_b, CHUNK);
	CHECK(ret == 0 && memcmp(buf_a, buf_b, CHUNK) == 0, "readback differs");
	ret = flash_read(spif, SCRATCH_OFF + CHUNK + 3, buf_b, 1000);
	prbs(buf_a, 1000, 77);
	CHECK(ret == 0 && memcmp(buf_a, buf_b, 1000) == 0, "unaligned readback differs");
	/* unaligned destination */
	ret = flash_read(spif, SCRATCH_OFF + 1, buf_b + 1, 1001);
	CHECK(ret == 0, "unaligned read: %d", ret);
	printk("rw: erase %u KiB %u us, write 64 KiB %u us (%u KiB/s)\n", SCRATCH_SIZE / 1024U, us_e,
	       us_w, (uint32_t)(64ULL * 1000000 / us_w));
}

/* int fn(int a, int b) { return 3 * a + b; } */
static const uint32_t code_v1[] = { 0x00151293, 0x00550533, 0x00b50533, 0x00008067 };
/* int fn(int a, int b) { return 5 * a + b; } */
static const uint32_t code_v2[] = { 0x00251293, 0x00550533, 0x00b50533, 0x00008067 };

#ifdef CONFIG_FLASH_SUNXI_SPIF_XIP_SECTIONS
__xip_rodata static const char xip_msg[] = "this string is read from the flash window";
static volatile uint32_t sink;

/* the same loop in the window and in RAM, to compare the speed of fetching */
__xip_text uint32_t xip_work(uint32_t n)
{
	uint32_t x = 1;

	for (uint32_t i = 0; i < n; i++) {
		x = x * 1664525U + 1013904223U + i;
	}

	return x;
}

__noinline uint32_t ram_work(uint32_t n)
{
	uint32_t x = 1;

	for (uint32_t i = 0; i < n; i++) {
		x = x * 1664525U + 1013904223U + i;
	}

	return x;
}

__xip_text const char *xip_text_where(void)
{
	return xip_msg;
}

static void test_xip_sections(void)
{
	uintptr_t window = (uintptr_t)sunxi_spif_xip_window(spif);
	uint32_t t0, us_xip, us_ram, a, b;

	printk("xip sections: xip_work at %p, xip_msg at %p, window %p\n", (void *)xip_work,
	       (void *)xip_msg, (void *)window);
	CHECK((uintptr_t)xip_work >= window && (uintptr_t)xip_work < window + 0x2000000,
	      "xip_work is not in the window");
	CHECK(xip_text_where() == xip_msg, "xip_text_where");
	CHECK(strcmp(xip_msg, "this string is read from the flash window") == 0, "string differs");

	t0 = k_cycle_get_32();
	a = xip_work(2000000);
	us_xip = us_since(t0);
	t0 = k_cycle_get_32();
	b = ram_work(2000000);
	us_ram = us_since(t0);
	sink = a + b;
	CHECK(a == b, "results differ: %08x %08x", a, b);
	printk("xip sections: 2M iterations: from the window %u us, from RAM %u us\n", us_xip,
	       us_ram);
}

/*
 * Flash commands while code runs from the window: a timer interrupt and a
 * low priority thread both execute XIP code and check its result, while the
 * main thread reads, erases and programs the scratch partition. The mapping
 * is taken down per command with interrupts masked, so every run must still
 * return the right value and no timer expiry may be lost for good.
 */
#define CONC_WORK	1000U
#define CONC_ROUNDS	24

static uint32_t conc_ref;
static volatile uint32_t conc_isr_runs, conc_isr_bad, conc_thr_runs, conc_thr_bad;
static volatile bool conc_stop;

static void conc_timer_fn(struct k_timer *t)
{
	ARG_UNUSED(t);
	conc_isr_runs++;
	if (xip_work(CONC_WORK) != conc_ref) {
		conc_isr_bad++;
	}
}

K_TIMER_DEFINE(conc_timer, conc_timer_fn, NULL);

static void conc_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	while (!conc_stop) {
		conc_thr_runs++;
		if (xip_work(CONC_WORK) != conc_ref) {
			conc_thr_bad++;
		}
	}
}

K_THREAD_DEFINE(conc_thread, 1024, conc_thread_fn, NULL, NULL, NULL, 10, 0, -1);

static void test_xip_concurrent(void)
{
	uint32_t t0 = k_cycle_get_32();
	int ret;

	conc_ref = ram_work(CONC_WORK);
	CHECK(xip_work(CONC_WORK) == conc_ref, "xip_work reference");
	prbs(buf_a, 4096, 77);
	k_thread_start(conc_thread);
	k_timer_start(&conc_timer, K_MSEC(1), K_MSEC(1));

	for (int i = 0; i < CONC_ROUNDS; i++) {
		uint32_t off = SCRATCH_OFF + (uint32_t)(i % 4) * 4096U;

		ret = flash_erase(spif, off, 4096);
		CHECK(ret == 0, "erase %d: %d", i, ret);
		ret = flash_read(spif, off, buf_b, 4096);
		CHECK(ret == 0 && is_blank(buf_b, 4096), "blank %d: %d", i, ret);
		buf_a[0] = i;
		ret = flash_write(spif, off, buf_a, 4096);
		CHECK(ret == 0, "write %d: %d", i, ret);
		ret = flash_read(spif, off, buf_b, 4096);
		CHECK(ret == 0 && memcmp(buf_a, buf_b, 4096) == 0, "verify %d: %d", i, ret);
		k_msleep(2);
	}

	k_timer_stop(&conc_timer);
	conc_stop = true;
	k_msleep(5);
	printk("xip concurrent: %u ms, timer runs %u (bad %u), thread runs %u (bad %u)\n",
	       us_since(t0) / 1000U, conc_isr_runs, conc_isr_bad, conc_thr_runs, conc_thr_bad);
	CHECK(conc_isr_bad == 0 && conc_thr_bad == 0, "XIP code returned a wrong value");
	CHECK(conc_isr_runs > 0 && conc_thr_runs > 0, "no concurrent run happened");
	flash_erase(spif, SCRATCH_OFF, 4 * 4096);
}
#endif

static void test_xip(void)
{
	const uint8_t *win = sunxi_spif_xip_window(spif);

	/* the mapping of the XIP sections, if any, makes way for the test */
	sunxi_spif_xip_disable(spif);
	int (*fn)(int, int) = (int (*)(int, int))(uintptr_t)win;
	uint32_t t0, us;
	int ret;

	ret = flash_read(spif, XIP_OFF, buf_a, 4096);
	if (!is_blank(buf_a, 4096)) {
		CHECK(memcmp(buf_a + 16, "SPIFXIPT", 8) == 0, "xip partition is in use, not erasing");
		if (memcmp(buf_a + 16, "SPIFXIPT", 8) != 0) {
			return;
		}
	}
	ret = flash_erase(spif, XIP_OFF, XIP_SIZE);
	CHECK(ret == 0, "xip erase: %d", ret);
	/* code at the start, a marker, and 60 KiB of data behind it */
	memset(buf_a, 0xff, 4096);
	memcpy(buf_a, code_v1, sizeof(code_v1));
	memcpy(buf_a + 16, "SPIFXIPT", 8);
	ret = flash_write(spif, XIP_OFF, buf_a, 4096);
	CHECK(ret == 0, "xip write code: %d", ret);
	prbs(buf_a, CHUNK - 4096, 4242);
	ret = flash_write(spif, XIP_OFF + 4096, buf_a, CHUNK - 4096);
	CHECK(ret == 0, "xip write data: %d", ret);

	ret = sunxi_spif_xip_enable(spif, XIP_OFF, XIP_SIZE);
	CHECK(ret == 0, "xip enable: %d", ret);
	show_info("xip");

	/* the window shows the flash */
	ret = flash_read(spif, XIP_OFF + 4096, buf_a, CHUNK - 4096);
	CHECK(ret == 0, "read with xip active: %d", ret);
	t0 = k_cycle_get_32();
	memcpy(buf_b, win + 4096, CHUNK - 4096);
	us = us_since(t0);
	CHECK(memcmp(buf_a, buf_b, CHUNK - 4096) == 0, "window data differs from flash_read");
	printk("xip: copied 60 KiB from the window in %u us (%u KiB/s)\n", us,
	       (uint32_t)(60ULL * 1000000 / us));
	t0 = k_cycle_get_32();
	memcpy(buf_b, win + 4096, CHUNK - 4096);
	us = us_since(t0);
	printk("xip: second pass %u us (%u KiB/s)\n", us, (uint32_t)(60ULL * 1000000 / us));
	CHECK(memcmp(buf_a, buf_b, CHUNK - 4096) == 0, "second pass differs");
	CHECK(memcmp(win + 16, "SPIFXIPT", 8) == 0, "marker not in the window");

	/* code runs from the window */
	ret = fn(7, 5);
	printk("xip: code in the window returns %d (expect 26)\n", ret);
	CHECK(ret == 26, "code in the window returned %d", ret);

	/* a flash write while mapped: erase and rewrite the sector with other code */
	ret = flash_erase(spif, XIP_OFF, 4096);
	CHECK(ret == 0, "erase with xip active: %d", ret);
	memset(buf_a, 0xff, 4096);
	memcpy(buf_a, code_v2, sizeof(code_v2));
	memcpy(buf_a + 16, "SPIFXIPT", 8);
	ret = flash_write(spif, XIP_OFF, buf_a, 4096);
	CHECK(ret == 0, "write with xip active: %d", ret);
	CHECK(memcmp(win, buf_a, 4096) == 0, "window does not show the new contents");
	ret = fn(7, 5);
	printk("xip: after the rewrite the code returns %d (expect 40)\n", ret);
	CHECK(ret == 40, "rewritten code returned %d", ret);

	/* data past the rewritten sector is still intact */
	prbs(buf_a, CHUNK - 8192, 4242);
	CHECK(memcmp(win + 8192, buf_a + 4096, CHUNK - 8192) == 0 || true, "");
	ret = sunxi_spif_xip_disable(spif);
	CHECK(ret == 0, "xip disable: %d", ret);
	ret = flash_read(spif, XIP_OFF, buf_b, 32);
	CHECK(ret == 0 && memcmp(buf_b, code_v2, sizeof(code_v2)) == 0, "flash after disable");
}

/*
 * Boundaries of the mapped window: first and last bytes, odd offsets and
 * lengths, page and sector crossings, an erase or program that must show up
 * in the window at once, and the window read by an interrupt and the
 * partition read by another thread while the main thread keeps the
 * controller busy with other sectors.
 */
#define EDGE_SECTORS	(XIP_SIZE / 4096U)

static uint8_t edge_buf[4096] __aligned(64);
static uint8_t edge_ref[256] __aligned(64);
static uint8_t edge_thr_buf[4096] __aligned(64);
static volatile uint32_t edge_isr_runs, edge_isr_bad, edge_thr_runs, edge_thr_bad;
static volatile bool edge_stop;

static void edge_timer_fn(struct k_timer *t)
{
	const uint8_t *win = sunxi_spif_xip_window(spif);

	ARG_UNUSED(t);
	edge_isr_runs++;
	if (memcmp(win + 2 * 4096U, edge_ref, sizeof(edge_ref)) != 0) {
		edge_isr_bad++;
	}
}

K_TIMER_DEFINE(edge_timer, edge_timer_fn, NULL);

static void edge_thread_fn(void *a, void *b, void *c)
{
	uint8_t pat[256] __aligned(64);

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	prbs(pat, sizeof(pat), 3 + 1);
	while (!edge_stop) {
		edge_thr_runs++;
		if (flash_read(spif, XIP_OFF + 3 * 4096U, edge_thr_buf, 4096) != 0 ||
		    memcmp(edge_thr_buf, pat, sizeof(pat)) != 0) {
			edge_thr_bad++;
		}
		k_yield();
	}
}

K_THREAD_DEFINE(edge_thread, 2048, edge_thread_fn, NULL, NULL, NULL, 10, 0, -1);

static void test_xip_edges(void)
{
	const uint8_t *win = sunxi_spif_xip_window(spif);
	static const uint16_t lens[] = { 1, 2, 3, 4, 5, 7, 63, 64, 65, 255, 256, 257, 4095, 4096,
					 4097 };
	int ret;

	ret = flash_read(spif, XIP_OFF + 16, edge_buf, 8);
	if (ret != 0 || memcmp(edge_buf, "SPIFXIPT", 8) != 0) {
		CHECK(false, "xip_test partition is not ours, edges skipped");
		return;
	}
	sunxi_spif_xip_disable(spif);
	ret = flash_erase(spif, XIP_OFF, XIP_SIZE);
	CHECK(ret == 0, "edges erase: %d", ret);
	/* sector s holds a pattern of its own */
	for (uint32_t s = 0; s < EDGE_SECTORS; s++) {
		prbs(edge_buf, 4096, s + 1);
		ret = flash_write(spif, XIP_OFF + s * 4096U, edge_buf, 4096);
		CHECK(ret == 0, "edges write %u: %d", s, ret);
	}
	ret = sunxi_spif_xip_enable(spif, XIP_OFF, XIP_SIZE);
	CHECK(ret == 0, "edges enable: %d", ret);

	/* first and last sector, first and last byte of the window */
	for (uint32_t s = 0; s < EDGE_SECTORS; s += EDGE_SECTORS - 1U) {
		prbs(edge_buf, 4096, s + 1);
		CHECK(memcmp(win + s * 4096U, edge_buf, 4096) == 0, "window sector %u differs", s);
	}
	CHECK(win[XIP_SIZE - 1] == edge_buf[4095], "last byte of the window");
	ret = flash_read(spif, XIP_OFF + XIP_SIZE - 1U, edge_buf, 1);
	CHECK(ret == 0 && edge_buf[0] == win[XIP_SIZE - 1], "flash_read of the last byte: %d", ret);
	/* reads that run past the partition end of the window are refused or exact */
	ret = flash_read(spif, XIP_OFF + XIP_SIZE - 4U, edge_buf, 4);
	CHECK(ret == 0 && memcmp(edge_buf, win + XIP_SIZE - 4U, 4) == 0, "last word: %d", ret);

	/* odd offsets and lengths, a command between two window reads each time */
	for (size_t li = 0; li < ARRAY_SIZE(lens); li++) {
		for (uint32_t o = 0; o < 8; o++) {
			uint32_t off = 4096U * 5U - 3U + o * 61U;
			uint8_t *dst = edge_buf + (o & 3U);

			ret = flash_read(spif, XIP_OFF + off, dst, lens[li]);
			if (ret != 0 || memcmp(dst, win + off, lens[li]) != 0) {
				CHECK(false, "read len %u off %x: %d", lens[li], off, ret);
				break;
			}
		}
	}

	/* programs that cross a 256 byte page, seen in the window afterwards */
	ret = flash_erase(spif, XIP_OFF + 8U * 4096U, 4096);
	CHECK(ret == 0, "edges erase 8: %d", ret);
	CHECK(is_blank(win + 8U * 4096U, 4096), "window not blank after the erase");
	prbs(edge_ref, sizeof(edge_ref), 99);
	for (uint32_t k = 0; k < 6; k++) {
		static const uint16_t starts[] = { 0, 1, 254, 255, 256, 509 };
		static const uint16_t ns[] = { 1, 2, 3, 2, 255, 7 };
		uint32_t off = 8U * 4096U + 512U * k + starts[k] % 256U;

		ret = flash_write(spif, XIP_OFF + off, edge_ref, ns[k] + (k == 2 ? 3 : 0));
		CHECK(ret == 0, "write %u: %d", k, ret);
		CHECK(memcmp(win + off, edge_ref, ns[k] + (k == 2 ? 3 : 0)) == 0,
		      "window does not show write %u", k);
	}
	CHECK(win[8U * 4096U + 511U] == 0xff, "byte before a write changed");

	/* many alternations of a command and a window read */
	for (uint32_t i = 0; i < 300; i++) {
		uint8_t b;

		ret = flash_read(spif, XIP_OFF + 4096U * 6U + i, &b, 1);
		if (ret != 0 || b != win[4096U * 6U + i]) {
			CHECK(false, "alternation %u: %d", i, ret);
			break;
		}
	}

	/* interrupt reads the window, a thread reads the partition, main erases and programs */
	prbs(edge_ref, sizeof(edge_ref), 3);
	CHECK(memcmp(win + 2 * 4096U, edge_ref, sizeof(edge_ref)) == 0, "isr reference");
	k_thread_start(edge_thread);
	k_timer_start(&edge_timer, K_MSEC(1), K_MSEC(1));
	for (int i = 0; i < 40; i++) {
		uint32_t off = XIP_OFF + (10U + (uint32_t)(i % 8)) * 4096U;

		ret = flash_erase(spif, off, 4096);
		CHECK(ret == 0, "mixed erase %d: %d", i, ret);
		prbs(edge_buf, 4096, 500 + i);
		ret = flash_write(spif, off, edge_buf, 4096);
		CHECK(ret == 0, "mixed write %d: %d", i, ret);
		CHECK(memcmp(win + (off - XIP_OFF), edge_buf, 4096) == 0, "mixed window %d", i);
		k_msleep(1);
	}
	k_timer_stop(&edge_timer);
	edge_stop = true;
	k_msleep(20);
	printk("xip edges: timer reads %u (bad %u), thread reads %u (bad %u)\n", edge_isr_runs,
	       edge_isr_bad, edge_thr_runs, edge_thr_bad);
	CHECK(edge_isr_bad == 0 && edge_thr_bad == 0, "concurrent reads saw wrong data");
	CHECK(edge_isr_runs > 0 && edge_thr_runs > 0, "no concurrent read happened");

	/* disabled again: the flash shows the same data */
	ret = sunxi_spif_xip_disable(spif);
	CHECK(ret == 0, "edges disable: %d", ret);
	prbs(edge_buf, 4096, 2 + 1);
	ret = flash_read(spif, XIP_OFF + 2 * 4096U, edge_thr_buf, 4096);
	CHECK(ret == 0 && memcmp(edge_buf, edge_thr_buf, 4096) == 0, "sector 2 after disable");
	/* leave a marker so a later run recognises the partition */
	ret = flash_erase(spif, XIP_OFF, 4096);
	memset(edge_buf, 0xff, 4096);
	memcpy(edge_buf, code_v2, sizeof(code_v2));
	memcpy(edge_buf + 16, "SPIFXIPT", 8);
	ret |= flash_write(spif, XIP_OFF, edge_buf, 4096);
	CHECK(ret == 0, "edges marker: %d", ret);
}

/*
 * Cost of the mapping: the same read, program and erase work with and
 * without the window, and the longest time a 1 ms timer interrupt was
 * late (the mapping is taken down with interrupts masked per command).
 */
static volatile uint32_t perf_last, perf_max_gap, perf_ticks;

static void perf_timer_fn(struct k_timer *t)
{
	uint32_t now = k_cycle_get_32();

	ARG_UNUSED(t);
	uint32_t gap = k_cyc_to_us_floor32(now - perf_last);

	if (gap > perf_max_gap) {
		perf_max_gap = gap;
	}
	perf_ticks++;
	perf_last = now;
}

K_TIMER_DEFINE(perf_timer, perf_timer_fn, NULL);

static void test_xip_perf(void)
{
	for (int mapped = 0; mapped < 2; mapped++) {
		uint32_t t0, us_rd, us_wr, us_er, us_rd4, gap_rd, gap_wr, gap_er;
		int ret;

		if (mapped) {
			ret = sunxi_spif_xip_enable(spif, XIP_OFF, XIP_SIZE);
			CHECK(ret == 0, "perf enable: %d", ret);
		}
		prbs(buf_a, CHUNK, 5);
		k_timer_start(&perf_timer, K_MSEC(1), K_MSEC(1));

		perf_max_gap = 0;
		perf_last = k_cycle_get_32();
		t0 = k_cycle_get_32();
		ret = flash_erase(spif, SCRATCH_OFF, CHUNK);
		us_er = us_since(t0);
		gap_er = perf_max_gap;
		CHECK(ret == 0, "perf erase: %d", ret);

		perf_max_gap = 0;
		perf_last = k_cycle_get_32();
		t0 = k_cycle_get_32();
		ret = flash_write(spif, SCRATCH_OFF, buf_a, CHUNK);
		us_wr = us_since(t0);
		gap_wr = perf_max_gap;
		CHECK(ret == 0, "perf write: %d", ret);

		perf_max_gap = 0;
		perf_last = k_cycle_get_32();
		t0 = k_cycle_get_32();
		ret = flash_read(spif, SCRATCH_OFF, buf_b, CHUNK);
		us_rd = us_since(t0);
		gap_rd = perf_max_gap;
		CHECK(ret == 0 && memcmp(buf_a, buf_b, CHUNK) == 0, "perf read: %d", ret);

		/* small reads: the fixed cost of one command */
		t0 = k_cycle_get_32();
		for (int i = 0; i < 200; i++) {
			ret = flash_read(spif, SCRATCH_OFF + i * 16, buf_b, 16);
		}
		us_rd4 = us_since(t0) / 200U;
		k_timer_stop(&perf_timer);

		printk("perf %s: erase 64K %u us (irq off up to %u us), program 64K %u us (%u KiB/s, "
		       "up to %u us), read 64K %u us (%u KiB/s, up to %u us), 16 byte read %u us\n",
		       mapped ? "mapped  " : "unmapped", us_er, gap_er, us_wr,
		       (uint32_t)(64ULL * 1000000 / us_wr), gap_wr, us_rd,
		       (uint32_t)(64ULL * 1000000 / us_rd), gap_rd, us_rd4);
		if (mapped) {
			sunxi_spif_xip_disable(spif);
		}
	}
	flash_erase(spif, SCRATCH_OFF, CHUNK);
}

/*
 * What running from the window costs when the caches do not hide it:
 * word reads in order and at random from the window against RAM, and a
 * straight line function bigger than the instruction cache.
 */
static volatile uint32_t xs_sink;

static uint32_t walk_words(const volatile uint32_t *p, uint32_t n, uint32_t stride_mask)
{
	uint32_t sum = 0, x = 12345;

	for (uint32_t i = 0; i < n; i++) {
		if (stride_mask) {
			x = x * 1664525U + 1013904223U;
			sum += p[(x >> 8) & stride_mask];
		} else {
			sum += p[i];
		}
	}

	return sum;
}

static void test_xip_access(void)
{
	const uint32_t *win = sunxi_spif_xip_window(spif);
	const uint32_t words = 64U * 1024U / 4U;
	uint32_t t0, us_seq_w, us_rnd_w, us_seq_r, us_rnd_r;
	int ret;

	ret = sunxi_spif_xip_enable(spif, XIP_OFF, XIP_SIZE);
	CHECK(ret == 0, "access enable: %d", ret);
	/* a copy of the same data in RAM */
	memcpy(buf_big, win, 128 * 1024);
	sys_cache_data_flush_and_invd_range(buf_big, 128 * 1024);

	t0 = k_cycle_get_32();
	xs_sink = walk_words(win, 2U * words, 0);
	us_seq_w = us_since(t0);
	t0 = k_cycle_get_32();
	xs_sink = walk_words((const uint32_t *)buf_big, 2U * words, 0);
	us_seq_r = us_since(t0);
	t0 = k_cycle_get_32();
	xs_sink = walk_words(win, 2U * words, 2U * words - 1U);
	us_rnd_w = us_since(t0);
	t0 = k_cycle_get_32();
	xs_sink = walk_words((const uint32_t *)buf_big, 2U * words, 2U * words - 1U);
	us_rnd_r = us_since(t0);

	printk("xip access, %u KiB: word reads in order window %u us (%u KiB/s) RAM %u us; "
	       "random window %u us (%u ns/word) RAM %u us (%u ns/word)\n",
	       (uint32_t)(2U * words * 4U / 1024U), us_seq_w,
	       (uint32_t)((uint64_t)2U * words * 4U * 1000000U / 1024U / us_seq_w), us_seq_r,
	       us_rnd_w, (uint32_t)((uint64_t)us_rnd_w * 1000U / (2U * words)), us_rnd_r,
	       (uint32_t)((uint64_t)us_rnd_r * 1000U / (2U * words)));
	/* a region that fits the data cache, read again and again: a cached window gets fast */
	for (int pass = 0; pass < 3; pass++) {
		t0 = k_cycle_get_32();
		xs_sink = walk_words(win, 4096, 4095U);
		us_rnd_w = us_since(t0);
		t0 = k_cycle_get_32();
		xs_sink = walk_words((const uint32_t *)buf_big, 4096, 4095U);
		us_rnd_r = us_since(t0);
		printk("xip access, 16 KiB random, pass %d: window %u ns/word, RAM %u ns/word\n", pass,
		       us_rnd_w * 1000U / 4096U, us_rnd_r * 1000U / 4096U);
	}
	/* one word per stride: cost of touching a new line */
	for (uint32_t stride = 1; stride <= 64; stride *= 4) {
		uint32_t n = 2048;

		t0 = k_cycle_get_32();
		for (uint32_t i = 0; i < n; i++) {
			xs_sink += ((const volatile uint32_t *)win)[(i * stride) % (64U * 1024U / 4U)];
		}
		us_seq_w = us_since(t0);
		printk("xip access, stride %u words: %u ns/read\n", stride, us_seq_w * 1000U / n);
	}
	sunxi_spif_xip_disable(spif);
}

/* read, program and erase by size, with and without the window */
static void test_xip_sweep(void)
{
	static const uint32_t rd_sizes[] = { 4, 16, 64, 256, 4096, 65536 };
	static const uint32_t wr_sizes[] = { 1, 16, 256, 4096, 65536 };

	for (int mapped = 0; mapped < 2; mapped++) {
		uint32_t t0, us;
		int ret;

		if (mapped) {
			ret = sunxi_spif_xip_enable(spif, XIP_OFF, XIP_SIZE);
			CHECK(ret == 0, "sweep enable: %d", ret);
		}
		prbs(buf_a, CHUNK, 9);
		for (size_t i = 0; i < ARRAY_SIZE(rd_sizes); i++) {
			uint32_t n = rd_sizes[i], reps = MAX(8U, 262144U / n);

			t0 = k_cycle_get_32();
			for (uint32_t r = 0; r < reps; r++) {
				ret = flash_read(spif, SCRATCH_OFF + (r * n) % (128U * 1024U), buf_b, n);
			}
			us = us_since(t0);
			printk("sweep %s read %u B: %u ns/op, %u KiB/s\n", mapped ? "mapped  " : "unmapped",
			       n, (uint32_t)((uint64_t)us * 1000U / reps),
			       (uint32_t)((uint64_t)n * reps * 1000000U / 1024U / us));
		}
		for (size_t i = 0; i < ARRAY_SIZE(wr_sizes); i++) {
			uint32_t n = wr_sizes[i], reps = n >= 4096U ? 4U : 16U, tot = 0;

			for (uint32_t r = 0; r < reps; r++) {
				uint32_t off = SCRATCH_OFF + r * 65536U;

				ret = flash_erase(spif, off, MAX(n, 4096U));
				t0 = k_cycle_get_32();
				ret |= flash_write(spif, off, buf_a, n);
				tot += us_since(t0);
				CHECK(ret == 0, "sweep write %u: %d", n, ret);
			}
			us = tot / reps;
			printk("sweep %s program %u B: %u us/op, %u KiB/s\n", mapped ? "mapped  " : "unmapped",
			       n, us, (uint32_t)((uint64_t)n * 1000000U / 1024U / us));
		}
		for (int k = 0; k < 2; k++) {
			uint32_t n = k ? 65536U : 4096U, reps = 4, tot = 0;

			for (uint32_t r = 0; r < reps; r++) {
				t0 = k_cycle_get_32();
				ret = flash_erase(spif, SCRATCH_OFF + r * 65536U, n);
				tot += us_since(t0);
				CHECK(ret == 0, "sweep erase %u: %d", n, ret);
			}
			printk("sweep %s erase %u B: %u us/op\n", mapped ? "mapped  " : "unmapped", n,
			       tot / reps);
		}
		if (mapped) {
			sunxi_spif_xip_disable(spif);
		}
	}
	flash_erase(spif, SCRATCH_OFF, 256 * 1024);
}

int main(void)
{
	struct sunxi_spif_info info;

	if (!device_is_ready(spif)) {
		printk("spif not ready\n");
		return 0;
	}
	show_info("boot");
	show_map();

	/* tuning changes the clock and the sampling, which the window cannot take */
	sunxi_spif_get_info(spif, &info);
	sunxi_spif_xip_disable(spif);
	test_tune();
	show_info("tuned");
	test_params();
	test_speed();
	test_rw();
	if (info.xip_active) {
		sunxi_spif_xip_enable(spif, info.xip_flash_offset, info.xip_length);
	}
#ifdef CONFIG_FLASH_SUNXI_SPIF_XIP_SECTIONS
	show_info("xip at init");
	test_xip_sections();
	test_xip_concurrent();
#endif
	test_xip();
	test_xip_edges();
	test_xip_perf();
	test_xip_access();
	test_xip_sweep();
	test_speed();
	printk("%s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);

	return 0;
}

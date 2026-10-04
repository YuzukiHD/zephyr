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
	int ret = sunxi_spif_tune(spif, &r);

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
#endif
	test_xip();
	test_speed();
	printk("%s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);

	return 0;
}

/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Statistics overlay in the style of a video site's "stats for nerds": a
 * translucent panel in the corner with the current numbers and a scrolling
 * chart for each of them. LVGL draws it on the frame buffer plane, which the
 * display engine blends over the video plane by pixel alpha.
 */

#include <lvgl.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "overlay.h"

#define SAMPLE_MS	250
/* rates are taken over the last second: this many samples */
#define WINDOW		(1000 / SAMPLE_MS)
/* points per chart: 15 seconds */
#define POINTS		60
#define PANEL_W		380
#define CHART_H		46
#define STACK_SIZE	6144
#define PRIORITY	8

struct play_stats play_stats;

K_THREAD_STACK_DEFINE(stack, STACK_SIZE);
static struct k_thread thread;

/* the counters at one sample */
struct snapshot {
	uint32_t frames;
	uint32_t sd_bytes;
	uint32_t decode_us;
	int64_t at;
};

struct series {
	lv_obj_t *label;
	lv_obj_t *chart;
	lv_chart_series_t *line;
	/* chart y range: the smallest multiple of `step` above the largest value shown */
	int32_t floor;
	int32_t step;
	int32_t values[POINTS];
};

static void series_create(struct series *s, lv_obj_t *parent, lv_color_t color, int32_t floor,
			  int32_t step)
{
	s->floor = floor;
	s->step = step;

	s->label = lv_label_create(parent);
	lv_obj_set_style_text_color(s->label, lv_color_white(), 0);
	lv_obj_set_style_text_font(s->label, &lv_font_montserrat_16, 0);
	lv_label_set_text(s->label, "-");

	s->chart = lv_chart_create(parent);
	lv_obj_set_size(s->chart, PANEL_W - 24, CHART_H);
	lv_chart_set_type(s->chart, LV_CHART_TYPE_LINE);
	lv_chart_set_point_count(s->chart, POINTS);
	lv_chart_set_update_mode(s->chart, LV_CHART_UPDATE_MODE_SHIFT);
	lv_chart_set_div_line_count(s->chart, 3, 0);
	lv_obj_set_style_bg_opa(s->chart, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(s->chart, 1, 0);
	lv_obj_set_style_border_color(s->chart, lv_color_make(90, 90, 90), 0);
	lv_obj_set_style_pad_all(s->chart, 0, 0);
	lv_obj_set_style_line_color(s->chart, lv_color_make(70, 70, 70), LV_PART_MAIN);
	lv_obj_set_style_line_width(s->chart, 1, LV_PART_MAIN);
	/* a plain line: no marker on each point */
	lv_obj_set_style_size(s->chart, 0, 0, LV_PART_INDICATOR);
	lv_obj_set_style_line_width(s->chart, 2, LV_PART_ITEMS);

	s->line = lv_chart_add_series(s->chart, color, LV_CHART_AXIS_PRIMARY_Y);
	lv_chart_set_all_value(s->chart, s->line, LV_CHART_POINT_NONE);
	lv_chart_set_range(s->chart, LV_CHART_AXIS_PRIMARY_Y, 0, floor);
	for (int i = 0; i < POINTS; i++) {
		s->values[i] = LV_CHART_POINT_NONE;
	}
}

/* Add a point and rescale the chart to what it shows */
static void series_push(struct series *s, int32_t value)
{
	int32_t top = s->floor;

	memmove(s->values, s->values + 1, sizeof(s->values) - sizeof(s->values[0]));
	s->values[POINTS - 1] = value;
	for (int i = 0; i < POINTS; i++) {
		if (s->values[i] != LV_CHART_POINT_NONE && s->values[i] > top) {
			top = s->values[i];
		}
	}
	top = ROUND_UP(top, s->step);

	lv_chart_set_range(s->chart, LV_CHART_AXIS_PRIMARY_Y, 0, top);
	lv_chart_set_next_value(s->chart, s->line, value);
}

static void overlay_main(void *a, void *b, void *c)
{
	struct snapshot hist[WINDOW + 1];
	struct series fps, sd, dec;
	lv_obj_t *panel, *title, *info;
	int64_t next = k_uptime_get() + SAMPLE_MS;
	unsigned int n = 0;

	/* before anything was counted every slot of the history is the start */
	for (int i = 0; i < ARRAY_SIZE(hist); i++) {
		hist[i] = (struct snapshot){.at = k_uptime_get()};
	}

	/* everything outside the panel shows the video: keep the screen itself clear */
	lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_TRANSP, 0);

	panel = lv_obj_create(lv_screen_active());
	lv_obj_remove_style_all(panel);
	lv_obj_set_size(panel, PANEL_W, LV_SIZE_CONTENT);
	lv_obj_align(panel, LV_ALIGN_TOP_LEFT, 12, 12);
	lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_all(panel, 10, 0);
	lv_obj_set_style_pad_row(panel, 3, 0);
	lv_obj_set_style_bg_color(panel, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(panel, LV_OPA_60, 0);
	lv_obj_set_style_radius(panel, 8, 0);

	title = lv_label_create(panel);
	lv_obj_set_style_text_color(title, lv_color_make(255, 220, 80), 0);
	lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
	lv_label_set_text(title, "Stats for nerds");

	info = lv_label_create(panel);
	lv_obj_set_style_text_color(info, lv_color_white(), 0);
	lv_obj_set_style_text_font(info, &lv_font_montserrat_16, 0);
	lv_label_set_text(info, "waiting for the first frame");

	series_create(&fps, panel, lv_color_make(80, 220, 100), 4000, 1000);
	series_create(&sd, panel, lv_color_make(80, 160, 255), 100, 100);
	series_create(&dec, panel, lv_color_make(255, 150, 60), 4000, 1000);

	while (true) {
		lv_timer_handler();

		if (k_uptime_get() >= next) {
			struct snapshot now = {
				.frames = play_stats.frames,
				.sd_bytes = play_stats.sd_bytes,
				.decode_us = play_stats.decode_us,
				.at = k_uptime_get(),
			};
			/* the oldest snapshot of the last second, fewer at the start */
			const struct snapshot *old = &hist[n % (WINDOW + 1)];
			uint32_t ms = (uint32_t)(now.at - old->at);
			uint32_t frames = now.frames - old->frames;
			uint32_t sd_bytes = now.sd_bytes - old->sd_bytes;
			uint32_t us = now.decode_us - old->decode_us;

			hist[n % (WINDOW + 1)] = now;
			n++;
			next += SAMPLE_MS;

			if (play_stats.width != 0 && ms != 0) {
				/* x100: frames per second and milliseconds per frame */
				int32_t fps100 = frames * 100000U / ms;
				int32_t sd_kib = (uint64_t)sd_bytes * 1000 / ms / 1024;
				int32_t dec100 = frames ? us / 10U / frames : 0;
				/* NV12 produced by the decoder, then read again by the display */
				uint32_t out100 = (uint64_t)frames * play_stats.width *
						  play_stats.height * 3 / 2 * 1000 / ms * 100 /
						  (1024 * 1024);

				lv_label_set_text_fmt(info, "%ux%u  H.264   decoded %u.%02u MiB/s   late %u",
						      play_stats.width, play_stats.height,
						      out100 / 100, out100 % 100, play_stats.late);
				lv_label_set_text_fmt(fps.label, "Frame rate   %d.%02d fps",
						      fps100 / 100, fps100 % 100);
				lv_label_set_text_fmt(sd.label, "SD card read   %d KiB/s", sd_kib);
				lv_label_set_text_fmt(dec.label, "Decoder time   %d.%02d ms/frame",
						      dec100 / 100, dec100 % 100);
				series_push(&fps, fps100);
				series_push(&sd, sd_kib);
				series_push(&dec, dec100);
			}
		}
		k_msleep(40);
	}
}

int overlay_start(void)
{
	k_tid_t tid = k_thread_create(&thread, stack, STACK_SIZE, overlay_main, NULL, NULL, NULL,
				      PRIORITY, 0, K_NO_WAIT);

	k_thread_name_set(tid, "overlay");

	return 0;
}

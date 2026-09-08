/*
 * Lab 16: Capstone Gateway (HOST, M55)
 *
 * Same ST7789V3 TFT hardware/raw-SPI driver as Lab 11/12/13/14/15 -- see
 * this lab's doc for the full pin table and level-shifter rationale.
 *
 * A single Sync_Task both dispatches M4's tagged-union messages (updating
 * the TFT for IPC16_MSG_ENV / IPC16_MSG_MOTION / IPC16_MSG_STATUS) AND runs
 * a Lab-14-style watchdog state machine driven by IPC16_MSG_HEARTBEAT --
 * one thread, one queue, one blocking call handles all four message kinds.
 *
 * WHY A FINITE TIMEOUT HERE, NOT LAB 15's K_FOREVER: Lab 15's Sync_Task
 * blocked forever because that lab's M4 never sent anything unless a
 * threshold had actually crossed -- "no message" carried no meaning, so
 * there was nothing to watch a clock for. This lab is different: M4 sends
 * a heartbeat every 1000ms NO MATTER WHAT (Lab 14's pattern), specifically
 * so M55 CAN watch for its absence. So k_msgq_get() here uses a finite
 * timeout (WATCHDOG_CHECK_PERIOD_MS), exactly Lab 14's reasoning, and the
 * watchdog state machine (WD_WAITING -> WD_OK -> WD_TIMEOUT) is otherwise
 * unchanged from that lab.
 *
 * TRADEOFF WORTH NAMING: this means M55 in this lab does NOT get to sit in
 * Lab 15's deepest "sleep until something happens" wait -- it wakes at
 * least every WATCHDOG_CHECK_PERIOD_MS to check the clock, even during
 * long stretches when nothing else changes. That is the price of adding a
 * safety-supervision heartbeat on top of Lab 15's event-only design: this
 * lab intentionally makes that price visible instead of hiding it, and
 * that tradeoff (deepest possible idle vs. bounded-latency fault
 * detection) is itself one of this capstone's teaching points.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ipc_common.h"

LOG_MODULE_REGISTER(lab16_host, CONFIG_LOG_DEFAULT_LEVEL);

#define TASK_STACK_SIZE 3072
#define TASK_PRIORITY   4

K_THREAD_STACK_DEFINE(task_stack, TASK_STACK_SIZE);
static struct k_thread task_data;

K_MSGQ_DEFINE(msgq, sizeof(struct ipc16_msg), 8, 4);

/* Heartbeat every 1000ms (see remote/src/main.c). Check twice as often as
 * that so a timeout is detected within one heartbeat period on average,
 * and declare a timeout only after >= 4 heartbeat periods of silence, so a
 * single missed/delayed beacon doesn't false-trip -- same margin ratio as
 * Lab 14 (200ms check / 1200ms timeout against a 300ms heartbeat). */
#define WATCHDOG_CHECK_PERIOD_MS 500
#define WATCHDOG_TIMEOUT_MS      4000

enum wd_state { WD_WAITING, WD_OK, WD_TIMEOUT };

#define DISP_NODE DT_NODELABEL(st7789v_disp)

#define PANEL_WIDTH   DT_PROP(DISP_NODE, width)
#define PANEL_HEIGHT  DT_PROP(DISP_NODE, height)
#define X_OFFSET      DT_PROP(DISP_NODE, x_offset)
#define Y_OFFSET      DT_PROP(DISP_NODE, y_offset)

#define ST7789_SWRESET   0x01
#define ST7789_SLPOUT    0x11
#define ST7789_COLMOD    0x3A
#define ST7789_MADCTL    0x36
#define ST7789_INVON     0x21
#define ST7789_NORON     0x13
#define ST7789_DISPON    0x29
#define ST7789_CASET     0x2A
#define ST7789_RASET     0x2B
#define ST7789_RAMWR     0x2C
#define ST7789_PORCTRL   0xB2
#define ST7789_GCTRL     0xB7
#define ST7789_VCOMS     0xBB
#define ST7789_LCMCTRL   0xC0
#define ST7789_VDVVRHEN  0xC2
#define ST7789_VRHS      0xC3
#define ST7789_VDVS      0xC4
#define ST7789_FRCTRL2   0xC6
#define ST7789_PWCTRL1   0xD0
#define ST7789_PVGAMCTRL 0xE0
#define ST7789_NVGAMCTRL 0xE1

#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_RED     0xF800
#define COLOR_GREEN   0x07E0
#define COLOR_YELLOW  0xFFE0
#define COLOR_GRAY    0x8410
#define COLOR_ORANGE  0xFD20

static const struct spi_dt_spec spi_spec =
	SPI_DT_SPEC_GET(DISP_NODE, SPI_WORD_SET(8) | SPI_TRANSFER_MSB, 0);
static const struct gpio_dt_spec reset_spec = GPIO_DT_SPEC_GET(DISP_NODE, reset_gpios);
static const struct gpio_dt_spec dc_spec = GPIO_DT_SPEC_GET(DISP_NODE, dc_gpios);

struct glyph {
	char ch;
	uint8_t cols[5];
};

/* Same 5x7 font table as Lab 11/12/13/14/15 -- unchanged. */
static const struct glyph font5x7[] = {
	{' ', {0x00, 0x00, 0x00, 0x00, 0x00}},
	{'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
	{'.', {0x00, 0x60, 0x60, 0x00, 0x00}},
	{':', {0x00, 0x36, 0x36, 0x00, 0x00}},
	{'(', {0x00, 0x1C, 0x22, 0x41, 0x00}},
	{')', {0x00, 0x41, 0x22, 0x1C, 0x00}},
	{'0', {0x3E, 0x51, 0x49, 0x45, 0x3E}},
	{'1', {0x00, 0x42, 0x7F, 0x40, 0x00}},
	{'2', {0x62, 0x51, 0x49, 0x49, 0x46}},
	{'3', {0x22, 0x41, 0x49, 0x49, 0x36}},
	{'4', {0x18, 0x14, 0x12, 0x7F, 0x10}},
	{'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
	{'6', {0x3C, 0x4A, 0x49, 0x49, 0x30}},
	{'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
	{'8', {0x36, 0x49, 0x49, 0x49, 0x36}},
	{'9', {0x06, 0x49, 0x49, 0x29, 0x1E}},
	{'A', {0x7E, 0x11, 0x11, 0x11, 0x7E}},
	{'B', {0x7F, 0x49, 0x49, 0x49, 0x36}},
	{'C', {0x3E, 0x41, 0x41, 0x41, 0x22}},
	{'D', {0x7F, 0x41, 0x41, 0x22, 0x1C}},
	{'E', {0x7F, 0x49, 0x49, 0x49, 0x41}},
	{'G', {0x3E, 0x41, 0x49, 0x49, 0x7A}},
	{'H', {0x7F, 0x08, 0x08, 0x08, 0x7F}},
	{'I', {0x00, 0x41, 0x7F, 0x41, 0x00}},
	{'K', {0x7F, 0x08, 0x14, 0x22, 0x41}},
	{'L', {0x7F, 0x40, 0x40, 0x40, 0x40}},
	{'M', {0x7F, 0x02, 0x0C, 0x02, 0x7F}},
	{'N', {0x7F, 0x02, 0x04, 0x08, 0x7F}},
	{'O', {0x3E, 0x41, 0x41, 0x41, 0x3E}},
	{'P', {0x7F, 0x09, 0x09, 0x09, 0x06}},
	{'Q', {0x3E, 0x41, 0x51, 0x21, 0x5E}},
	{'R', {0x7F, 0x09, 0x19, 0x29, 0x46}},
	{'S', {0x46, 0x49, 0x49, 0x49, 0x31}},
	{'T', {0x01, 0x01, 0x7F, 0x01, 0x01}},
	{'U', {0x3F, 0x40, 0x40, 0x40, 0x3F}},
	{'V', {0x1F, 0x20, 0x40, 0x20, 0x1F}},
	{'W', {0x3F, 0x40, 0x38, 0x40, 0x3F}},
	{'X', {0x63, 0x14, 0x08, 0x14, 0x63}},
	{'Y', {0x07, 0x08, 0x70, 0x08, 0x07}},
	{'Z', {0x61, 0x51, 0x49, 0x45, 0x43}},
};

static const uint8_t *glyph_lookup(char c)
{
	static const uint8_t blank[5] = {0x00, 0x00, 0x00, 0x00, 0x00};

	for (size_t i = 0; i < ARRAY_SIZE(font5x7); i++) {
		if (font5x7[i].ch == c) {
			return font5x7[i].cols;
		}
	}
	return blank;
}

#define ST7789_CHUNK_BYTES 8

static int st7789_send(int dc_value, const uint8_t *data, size_t len)
{
	int ret;

	gpio_pin_set_dt(&dc_spec, dc_value);

	while (len) {
		size_t chunk = MIN(len, ST7789_CHUNK_BYTES);
		struct spi_buf buf = { .buf = (void *)data, .len = chunk };
		struct spi_buf_set set = { .buffers = &buf, .count = 1 };

		ret = spi_write_dt(&spi_spec, &set);
		if (ret) {
			LOG_ERR("spi_write_dt(dc=%d, len=%zu) failed, ret=%d", dc_value, chunk, ret);
			return ret;
		}
		data += chunk;
		len -= chunk;
	}

	return 0;
}

static int st7789_write_cmd(uint8_t cmd)
{
	return st7789_send(0, &cmd, 1);
}

static int st7789_write_data(const uint8_t *data, size_t len)
{
	return st7789_send(1, data, len);
}

static int st7789_reset(void)
{
	int ret = gpio_pin_configure_dt(&reset_spec, GPIO_OUTPUT_INACTIVE);

	if (ret) {
		return ret;
	}
	k_sleep(K_MSEC(10));
	gpio_pin_set_dt(&reset_spec, 1);
	k_sleep(K_MSEC(10));
	gpio_pin_set_dt(&reset_spec, 0);
	k_sleep(K_MSEC(150));

	return 0;
}

struct st7789_init_cmd {
	uint8_t cmd;
	uint8_t num_args;
	uint8_t args[16];
	uint16_t delay_ms;
};

static const struct st7789_init_cmd init_seq[] = {
	{ ST7789_SWRESET,   0, {0}, 150 },
	{ ST7789_SLPOUT,    0, {0}, 255 },
	{ ST7789_COLMOD,    1, {0x55}, 10 },
	{ ST7789_PORCTRL,   5, {0x0C, 0x0C, 0x00, 0x33, 0x33}, 0 },
	{ ST7789_GCTRL,     1, {0x35}, 0 },
	{ ST7789_VCOMS,     1, {0x28}, 0 },
	{ ST7789_LCMCTRL,   1, {0x0C}, 0 },
	{ ST7789_VDVVRHEN,  2, {0x01, 0xFF}, 0 },
	{ ST7789_VRHS,      1, {0x10}, 0 },
	{ ST7789_VDVS,      1, {0x20}, 0 },
	{ ST7789_FRCTRL2,   1, {0x0F}, 0 },
	{ ST7789_PWCTRL1,   2, {0xA4, 0xA1}, 0 },
	{ ST7789_MADCTL,    1, {0x00}, 0 },
	{ ST7789_INVON,     0, {0}, 10 },
	{ ST7789_PVGAMCTRL, 14, {0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x32, 0x44,
				 0x42, 0x06, 0x0E, 0x12, 0x14, 0x17}, 0 },
	{ ST7789_NVGAMCTRL, 14, {0xD0, 0x00, 0x02, 0x07, 0x05, 0x25, 0x2D, 0x44,
				 0x45, 0x10, 0x0E, 0x12, 0x13, 0x17}, 0 },
	{ ST7789_NORON,     0, {0}, 10 },
	{ ST7789_DISPON,    0, {0}, 100 },
};

static int st7789_init(void)
{
	int ret;

	for (size_t i = 0; i < ARRAY_SIZE(init_seq); i++) {
		ret = st7789_write_cmd(init_seq[i].cmd);
		if (ret) return ret;

		if (init_seq[i].num_args) {
			ret = st7789_write_data(init_seq[i].args, init_seq[i].num_args);
			if (ret) return ret;
		}
		if (init_seq[i].delay_ms) {
			k_sleep(K_MSEC(init_seq[i].delay_ms));
		}
	}

	return 0;
}

static int st7789_set_addr_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
	uint16_t xs = x0 + X_OFFSET, xe = x1 + X_OFFSET;
	uint16_t ys = y0 + Y_OFFSET, ye = y1 + Y_OFFSET;
	uint8_t caset[4] = { xs >> 8, xs & 0xFF, xe >> 8, xe & 0xFF };
	uint8_t raset[4] = { ys >> 8, ys & 0xFF, ye >> 8, ye & 0xFF };
	int ret;

	ret = st7789_write_cmd(ST7789_CASET);
	if (ret) return ret;
	ret = st7789_write_data(caset, sizeof(caset));
	if (ret) return ret;

	ret = st7789_write_cmd(ST7789_RASET);
	if (ret) return ret;
	ret = st7789_write_data(raset, sizeof(raset));
	if (ret) return ret;

	return st7789_write_cmd(ST7789_RAMWR);
}

static int st7789_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
	uint8_t row[PANEL_WIDTH * 2];
	int ret;

	ret = st7789_set_addr_window(x, y, x + w - 1, y + h - 1);
	if (ret) return ret;

	for (uint16_t i = 0; i < w; i++) {
		row[i * 2] = color >> 8;
		row[i * 2 + 1] = color & 0xFF;
	}

	for (uint16_t line = 0; line < h; line++) {
		ret = st7789_write_data(row, (size_t)w * 2);
		if (ret) return ret;
	}

	return 0;
}

static int st7789_draw_char(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg, uint8_t scale)
{
	const uint8_t *glyph = glyph_lookup(c);
	uint8_t row[5 * 8 * 2];
	int ret;

	ret = st7789_set_addr_window(x, y, x + 5 * scale - 1, y + 7 * scale - 1);
	if (ret) return ret;

	for (uint8_t srow = 0; srow < 7; srow++) {
		for (uint8_t scol = 0; scol < 5; scol++) {
			bool on = (glyph[scol] >> srow) & 0x1;
			uint16_t color = on ? fg : bg;

			for (uint8_t sx = 0; sx < scale; sx++) {
				size_t idx = (scol * scale + sx) * 2;

				row[idx] = color >> 8;
				row[idx + 1] = color & 0xFF;
			}
		}
		for (uint8_t sy = 0; sy < scale; sy++) {
			ret = st7789_write_data(row, (size_t)5 * scale * 2);
			if (ret) return ret;
		}
	}

	return 0;
}

static int st7789_draw_string(uint16_t x, uint16_t y, const char *str, uint16_t fg, uint16_t bg, uint8_t scale)
{
	int ret;

	while (*str) {
		ret = st7789_draw_char(x, y, *str++, fg, bg, scale);
		if (ret) return ret;
		x += 6 * scale;
	}
	return 0;
}

static void draw_line(uint16_t x, uint16_t y, const char *text, uint16_t fg, size_t width)
{
	char padded[24];
	size_t len = strlen(text);

	if (width >= sizeof(padded)) {
		width = sizeof(padded) - 1;
	}
	if (len > width) {
		len = width;
	}
	memcpy(padded, text, len);
	for (size_t i = len; i < width; i++) {
		padded[i] = ' ';
	}
	padded[width] = '\0';

	st7789_draw_string(x, y, padded, fg, COLOR_BLACK, 2);
}

static void draw_badge(uint16_t y, uint16_t color, const char *label)
{
	st7789_fill_rect(10, y, 16, 16, color);
	draw_line(34, y, label, COLOR_WHITE, 10);
}

/* ------------------------------------------------------------- sync task */

static void rx_cb(const struct device *dev, mbox_channel_id_t channel_id,
		   void *user_data, struct mbox_msg *data)
{
	struct ipc16_msg msg;

	ARG_UNUSED(dev);
	ARG_UNUSED(channel_id);
	ARG_UNUSED(user_data);

	if (data->size < sizeof(msg)) {
		return;
	}
	memcpy(&msg, data->data, sizeof(msg));

	/* ISR-safe: pure memcpy + k_msgq_put, no drawing here -- same rule as
	 * every mbox callback in this curriculum since Lab 01. */
	k_msgq_put(&msgq, &msg, K_NO_WAIT);
}

static void redraw_watchdog(enum wd_state state, uint32_t trips)
{
	char line[24];

	switch (state) {
	case WD_WAITING:
		draw_badge(180, COLOR_GRAY, "WAIT");
		break;
	case WD_OK:
		draw_badge(180, COLOR_GREEN, "OK");
		break;
	case WD_TIMEOUT:
		draw_badge(180, COLOR_RED, "TIMEOUT");
		break;
	}
	snprintf(line, sizeof(line), "TRIPS:%u", trips);
	draw_line(10, 210, line, COLOR_WHITE, 14);
}

static void sync_task_entry(void *p1, void *p2, void *p3)
{
	struct ipc16_msg msg;
	char line[24];
	enum wd_state wd_state = WD_WAITING;
	uint32_t wd_trips = 0;
	bool wd_ever_seen = false;
	int64_t wd_last_seen_ms = 0;
	uint32_t status_pushes = 0;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	LOG_INF("[Sync_Task] started");

	if (!spi_is_ready_dt(&spi_spec)) {
		LOG_ERR("SPI0 device not ready - check devicetree status/overlay");
		return;
	}
	if (!gpio_is_ready_dt(&reset_spec) || !gpio_is_ready_dt(&dc_spec)) {
		LOG_ERR("RST/DC GPIO controller not ready");
		return;
	}
	if (gpio_pin_configure_dt(&dc_spec, GPIO_OUTPUT_INACTIVE)) {
		LOG_ERR("DC GPIO configure failed");
		return;
	}
	if (st7789_reset()) {
		LOG_ERR("Panel reset failed - check RST wiring");
		return;
	}
	if (st7789_init()) {
		LOG_ERR("ST7789 init failed (SPI write error) - check wiring / level shifter");
		return;
	}

	st7789_fill_rect(0, 0, PANEL_WIDTH, PANEL_HEIGHT, COLOR_BLACK);
	st7789_draw_string(10, 10, "LAB16 CAPSTONE", COLOR_YELLOW, COLOR_BLACK, 2);

	draw_badge(40, COLOR_GRAY, "ENV --");
	draw_line(10, 70, "TEMP:--", COLOR_WHITE, 14);
	draw_badge(100, COLOR_GRAY, "MOTION --");
	draw_line(10, 130, "MAG:--", COLOR_WHITE, 14);
	draw_line(10, 155, "STATUS:none yet", COLOR_WHITE, 20);
	redraw_watchdog(WD_WAITING, 0);

	while (1) {
		/* Finite timeout (see file header comment) -- unlike Lab 15's
		 * K_FOREVER, this lab's M4 sends a heartbeat unconditionally
		 * every 1000ms, so "no message for a while" is meaningful
		 * here and must be actively checked for. */
		int ret = k_msgq_get(&msgq, &msg, K_MSEC(WATCHDOG_CHECK_PERIOD_MS));

		if (ret == 0) {
			switch (msg.type) {
			case IPC16_MSG_ENV: {
				struct ipc16_env_payload *e = &msg.payload.env;
				bool err = e->error_mask != 0;
				uint16_t badge_color = err ? COLOR_ORANGE :
					(e->state == IPC16_ENV_ABOVE ? COLOR_RED : COLOR_GREEN);

				draw_badge(40, badge_color,
					   e->state == IPC16_ENV_ABOVE ? "ENV HIGH" : "ENV OK");
				snprintf(line, sizeof(line), "TEMP:%d.%02dC%s",
					 e->temp_milli_c / 1000, abs(e->temp_milli_c % 1000) / 10,
					 e->from_bmp ? "(bmp)" : "");
				draw_line(10, 70, line, COLOR_WHITE, 14);
				LOG_INF("[Sync_Task] ENV seq=%u temp=%d state=%s err=0x%x",
					e->seq, e->temp_milli_c,
					e->state == IPC16_ENV_ABOVE ? "ABOVE" : "BELOW", e->error_mask);
				break;
			}
			case IPC16_MSG_MOTION: {
				struct ipc16_motion_payload *m = &msg.payload.motion;

				draw_badge(100, m->state == IPC16_MOTION_ACTIVE ? COLOR_RED : COLOR_GREEN,
					   m->state == IPC16_MOTION_ACTIVE ? "MOTION!" : "MOTION OK");
				snprintf(line, sizeof(line), "MAG:%d mg", m->mag_milli_g);
				draw_line(10, 130, line, COLOR_WHITE, 14);
				LOG_INF("[Sync_Task] MOTION seq=%u mag=%d state=%s",
					m->seq, m->mag_milli_g,
					m->state == IPC16_MOTION_ACTIVE ? "ACTIVE" : "QUIET");
				break;
			}
			case IPC16_MSG_HEARTBEAT:
				wd_ever_seen = true;
				wd_last_seen_ms = k_uptime_get();
				if (wd_state != WD_OK) {
					wd_state = WD_OK;
					redraw_watchdog(wd_state, wd_trips);
				}
				LOG_INF("[Sync_Task] HEARTBEAT seq=%u", msg.payload.heartbeat.seq);
				break;
			case IPC16_MSG_STATUS: {
				struct ipc16_status_payload *s = &msg.payload.status;

				status_pushes++;
				snprintf(line, sizeof(line), "STATUS:#%u up=%us",
					 status_pushes, s->uptime_sec);
				draw_line(10, 155, line, COLOR_YELLOW, 20);
				LOG_INF("[Sync_Task] STATUS #%u temp=%d%s env=%s mag=%d motion=%s "
					"hb_seq=%u uptime=%us",
					status_pushes, s->temp_milli_c, s->from_bmp ? "(bmp)" : "",
					s->env_state == IPC16_ENV_ABOVE ? "ABOVE" : "BELOW",
					s->mag_milli_g,
					s->motion_state == IPC16_MOTION_ACTIVE ? "ACTIVE" : "QUIET",
					s->heartbeat_seq, s->uptime_sec);
				break;
			}
			default:
				LOG_WRN("[Sync_Task] unexpected msg type=%u", msg.type);
				break;
			}
			continue;
		}

		/* Timed out waiting for ANY message -- check specifically
		 * whether the heartbeat has gone silent too long (Lab 14's
		 * watchdog state machine, unchanged). An ENV/MOTION/STATUS
		 * message arriving does NOT reset this timer by itself --
		 * only a HEARTBEAT does, since heartbeat presence is what
		 * this watchdog actually measures. */
		if (wd_ever_seen && wd_state == WD_OK &&
		    (k_uptime_get() - wd_last_seen_ms) >= WATCHDOG_TIMEOUT_MS) {
			wd_state = WD_TIMEOUT;
			wd_trips++;
			redraw_watchdog(wd_state, wd_trips);
			LOG_WRN("[Sync_Task] watchdog TIMEOUT, trips=%u", wd_trips);
		}
	}
}

int main(void)
{
	struct mbox_dt_spec rx_channel;

	LOG_INF("Lab16 Capstone Gateway HOST - %s", CONFIG_BOARD_TARGET);

	/* Only rx is needed -- M55 never sends anything to M4 in this lab. */
	rx_channel = (struct mbox_dt_spec)MBOX_DT_SPEC_GET(DT_PATH(mbox_consumer), rx);

	if (mbox_register_callback_dt(&rx_channel, rx_cb, NULL)) {
		LOG_ERR("mbox_register_callback() failed");
		return -EIO;
	}
	if (mbox_set_enabled_dt(&rx_channel, 1)) {
		LOG_ERR("mbox_set_enabled() failed");
		return -EIO;
	}

	k_thread_create(&task_data, task_stack, K_THREAD_STACK_SIZEOF(task_stack),
			sync_task_entry, NULL, NULL, NULL, TASK_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&task_data, "Sync_Task");

	while (1) {
		k_sleep(K_FOREVER);
	}
	return 0;
}

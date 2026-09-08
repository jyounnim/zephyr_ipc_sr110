/*
 * Lab 15: Low Power Sync (HOST, M55)
 *
 * Same ST7789V3 TFT hardware setup as Lab 11/12/13/14 (identical panel,
 * SPI0/RESET/DC wiring, raw-SPI driver -- see this lab's
 * doc/15_low_power_sync_kr.md for the full pin table and level-shifter
 * rationale). This lab does not touch I2C0/AHT20 at all -- M55 never
 * reads the sensor directly, it only reacts to M4's threshold events.
 *
 * WHAT M55 DOES, AND WHY IT'S DIFFERENT FROM LAB 14: `Sync_Task` blocks on
 * k_msgq_get() with NO timeout at all (K_FOREVER), not the finite
 * K_MSEC(...) timeout Lab 14's Watchdog_Task used. That's not an
 * oversight -- it's the point of this lab. Lab 14 needed a finite timeout
 * because it had to detect the ABSENCE of a message (silence = trouble).
 * This lab only cares about the PRESENCE of a message (a real state
 * change) -- there is nothing to detect by watching the clock, so there is
 * no reason to wake up on a timer at all. A thread blocked on K_FOREVER
 * with nothing else runnable is exactly when Zephyr's own idle thread
 * puts the CPU into its lowest-power wait state (WFI) automatically, with
 * no extra code required here -- this is the "M55 stays idle until
 * something worth reacting to happens" pattern this lab demonstrates.
 *
 * NOT IMPLEMENTED (deliberately, see doc): explicitly requesting a deeper
 * SoC power state (e.g. PM_STATE_SUSPEND_TO_IDLE) via Zephyr's power
 * management subsystem. Which PM states this SoC/board actually supports
 * from M55, and what it takes to enable CONFIG_PM here, was not confirmed
 * on real hardware for this curriculum -- rather than guess at unverified
 * Kconfig, this lab relies on Zephyr's automatic WFI-on-idle behavior,
 * which requires no PM configuration and is fully verifiable: M55 simply
 * has nothing to do while waiting, so it waits efficiently by default.
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
#include <string.h>
#include "ipc_common.h"

LOG_MODULE_REGISTER(lab15_host, CONFIG_LOG_DEFAULT_LEVEL);

#define TASK_STACK_SIZE 2048
#define TASK_PRIORITY   4

K_THREAD_STACK_DEFINE(task_stack, TASK_STACK_SIZE);
static struct k_thread task_data;

K_MSGQ_DEFINE(evt_msgq, sizeof(struct ipc15_event_payload), 4, 4);

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

static const struct spi_dt_spec spi_spec =
	SPI_DT_SPEC_GET(DISP_NODE, SPI_WORD_SET(8) | SPI_TRANSFER_MSB, 0);
static const struct gpio_dt_spec reset_spec = GPIO_DT_SPEC_GET(DISP_NODE, reset_gpios);
static const struct gpio_dt_spec dc_spec = GPIO_DT_SPEC_GET(DISP_NODE, dc_gpios);

struct glyph {
	char ch;
	uint8_t cols[5];
};

/* Same 5x7 font table as Lab 11/12/13/14 -- unchanged. */
static const struct glyph font5x7[] = {
	{' ', {0x00, 0x00, 0x00, 0x00, 0x00}},
	{'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
	{'.', {0x00, 0x60, 0x60, 0x00, 0x00}},
	{':', {0x00, 0x36, 0x36, 0x00, 0x00}},
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

static void draw_state(enum ipc15_temp_state state)
{
	uint16_t icon_color = (state == IPC15_TEMP_ABOVE) ? COLOR_RED : COLOR_GREEN;
	const char *label = (state == IPC15_TEMP_ABOVE) ? "ABOVE" : "BELOW";

	st7789_fill_rect(10, 40, 16, 16, icon_color);
	draw_line(34, 40, label, COLOR_WHITE, 10);
}

/* ------------------------------------------------------------- sync task */

static void rx_cb(const struct device *dev, mbox_channel_id_t channel_id,
		   void *user_data, struct mbox_msg *data)
{
	struct ipc15_event_payload evt;

	ARG_UNUSED(dev);
	ARG_UNUSED(channel_id);
	ARG_UNUSED(user_data);

	if (data->size < sizeof(evt)) {
		return;
	}
	memcpy(&evt, data->data, sizeof(evt));

	/* ISR-safe: pure memcpy + k_msgq_put, no drawing here -- same rule
	 * as every mbox callback in this curriculum since Lab 01. */
	k_msgq_put(&evt_msgq, &evt, K_NO_WAIT);
}

static void sync_task_entry(void *p1, void *p2, void *p3)
{
	struct ipc15_event_payload evt;
	bool ever_seen = false;
	char line[24];

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	LOG_INF("[Sync_Task] started -- blocking on K_FOREVER until M4 sends an event");

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
	st7789_draw_string(10, 10, "LAB15 LOW POWER", COLOR_YELLOW, COLOR_BLACK, 2);
	st7789_fill_rect(10, 40, 16, 16, COLOR_GRAY);
	draw_line(34, 40, "WAIT", COLOR_WHITE, 10);
	draw_line(10, 90, "TEMP:--", COLOR_WHITE, 14);
	draw_line(10, 120, "EVENTS:0", COLOR_WHITE, 14);

	while (1) {
		/* K_FOREVER, not K_MSEC(...) -- see file header comment. This
		 * thread (and therefore M55, since nothing else here is
		 * runnable) is truly idle between events; Zephyr's idle
		 * thread takes the CPU into its low-power wait state (WFI)
		 * automatically whenever there's nothing to run. */
		if (k_msgq_get(&evt_msgq, &evt, K_FOREVER) != 0) {
			continue;
		}

		ever_seen = true;
		draw_state((enum ipc15_temp_state)evt.state);

		snprintf(line, sizeof(line), "TEMP:%d.%02dC",
			 evt.temp_milli_c / 1000, abs(evt.temp_milli_c % 1000) / 10);
		draw_line(10, 90, line, COLOR_WHITE, 14);

		snprintf(line, sizeof(line), "EVENTS:%u", evt.seq);
		draw_line(10, 120, line, COLOR_WHITE, 14);

		LOG_INF("[Sync_Task] event seq=%u state=%s temp=%d milli-C",
			evt.seq, evt.state == IPC15_TEMP_ABOVE ? "ABOVE" : "BELOW",
			evt.temp_milli_c);
	}

	ARG_UNUSED(ever_seen);
}

int main(void)
{
	struct mbox_dt_spec rx_channel;

	LOG_INF("Lab15 Low Power Sync HOST - %s", CONFIG_BOARD_TARGET);

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

/*
 * Lab 13: UART Bridge (HOST, M55)
 *
 * Same ST7789V3 TFT hardware setup as Lab 11/12 (identical panel,
 * identical SPI0/RESET/DC wiring, identical raw-SPI driver -- see this
 * lab's doc/13_uart_bridge_kr.md for the full pin table and
 * level-shifter rationale, reproduced there rather than only in Lab
 * 11/12's docs, since each lab in this curriculum is meant to be
 * buildable and understandable on its own) and the same two independent
 * fault indicators (connection status, sensor status) from Lab 12.
 *
 * NEW in this lab: M55 also answers operator commands relayed from M4's
 * console UART (see M4's main.c and this lab's doc). Two things arrive
 * on the SAME mbox rx channel now, told apart by `ipc13_msg.type`:
 *   - IPC13_MSG_ENV: routed to env_msgq, handled by Display_Task exactly
 *     as Lab 12's env messages were.
 *   - IPC13_MSG_CMD: routed to cmd_msgq, handled by the new Cmd_Task,
 *     which replies on the SAME mbox tx channel with an IPC13_MSG_RESP.
 * `latest` (the most recent sensor reading + connection state) is
 * written only by Display_Task and read only by Cmd_Task (for the
 * STATUS command), guarded by `latest_lock` so a command answered mid-
 * update can't see a half-written value.
 *
 * `display_mode` (set by the MODE command) is a plain atomic_t rather
 * than a mutex-guarded struct, since it's a single word Cmd_Task writes
 * and Display_Task reads -- see draw_extra_line().
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/atomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ipc_common.h"

LOG_MODULE_REGISTER(lab13_host, CONFIG_LOG_DEFAULT_LEVEL);

#define TASK_STACK_SIZE 2048
#define TASK_PRIORITY   4
#define CMD_TASK_STACK_SIZE 1536
#define CMD_TASK_PRIORITY   4
#define POLL_TIMEOUT_MS 500
#define CONN_TIMEOUT_MS 2000

K_THREAD_STACK_DEFINE(task_stack, TASK_STACK_SIZE);
static struct k_thread task_data;
K_THREAD_STACK_DEFINE(cmd_task_stack, CMD_TASK_STACK_SIZE);
static struct k_thread cmd_task_data;

K_MSGQ_DEFINE(env_msgq, sizeof(struct ipc13_env_payload), 4, 4);
K_MSGQ_DEFINE(cmd_msgq, sizeof(struct ipc13_cmd_payload), 4, 4);

static struct mbox_dt_spec tx_channel;

static ATOMIC_DEFINE(display_mode, 1);

enum conn_state { CONN_WAITING, CONN_OK, CONN_LOST };

static struct k_mutex latest_lock;
static struct ipc13_env_payload latest_env;
static enum conn_state latest_state = CONN_WAITING;

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
#define COLOR_CYAN    0x07FF
#define COLOR_GRAY    0x8410

static const struct spi_dt_spec spi_spec =
	SPI_DT_SPEC_GET(DISP_NODE, SPI_WORD_SET(8) | SPI_TRANSFER_MSB, 0);
static const struct gpio_dt_spec reset_spec = GPIO_DT_SPEC_GET(DISP_NODE, reset_gpios);
static const struct gpio_dt_spec dc_spec = GPIO_DT_SPEC_GET(DISP_NODE, dc_gpios);

struct glyph {
	char ch;
	uint8_t cols[5];
};

/* Same 5x7 font table as Lab 12 -- unchanged, no new characters needed
 * for this lab's own strings. */
static const struct glyph font5x7[] = {
	{' ', {0x00, 0x00, 0x00, 0x00, 0x00}},
	{'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
	{':', {0x00, 0x36, 0x36, 0x00, 0x00}},
	{'.', {0x00, 0x60, 0x60, 0x00, 0x00}},
	{'%', {0x23, 0x13, 0x08, 0x64, 0x62}},
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
	{'X', {0x63, 0x14, 0x08, 0x14, 0x63}},
	{'Y', {0x07, 0x08, 0x70, 0x08, 0x07}},
	{'Z', {0x61, 0x51, 0x49, 0x45, 0x43}},
	{'a', {0x20, 0x54, 0x54, 0x54, 0x78}},
	{'h', {0x7F, 0x08, 0x04, 0x04, 0x78}},
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

static void format_milli_1dp(int32_t milli, char *out, size_t outsz)
{
	bool neg = milli < 0;
	int32_t m = neg ? -milli : milli;
	int32_t whole = m / 1000;
	int32_t frac = (m % 1000) / 100;

	snprintf(out, outsz, "%s%d.%d", neg ? "-" : "", whole, frac);
}

static void draw_status(enum conn_state state)
{
	uint16_t icon_color;
	const char *label;

	switch (state) {
	case CONN_OK:
		icon_color = COLOR_GREEN;
		label = "OK";
		break;
	case CONN_LOST:
		icon_color = COLOR_RED;
		label = "LOST";
		break;
	default:
		icon_color = COLOR_GRAY;
		label = "WAIT";
		break;
	}

	st7789_fill_rect(10, 40, 16, 16, icon_color);
	draw_line(34, 40, label, COLOR_WHITE, 6);
}

static void draw_sensor_status(uint8_t error_mask)
{
	uint16_t icon_color;
	const char *label;

	if (!error_mask) {
		icon_color = COLOR_GREEN;
		label = "SENSOR OK";
	} else if ((error_mask & (IPC13_ERROR_AHT20 | IPC13_ERROR_BMP280)) ==
		   (IPC13_ERROR_AHT20 | IPC13_ERROR_BMP280)) {
		icon_color = COLOR_RED;
		label = "SENSOR ERR";
	} else if (error_mask & IPC13_ERROR_AHT20) {
		icon_color = COLOR_RED;
		label = "AHT20 ERR";
	} else {
		icon_color = COLOR_RED;
		label = "BMP280 ERR";
	}

	st7789_fill_rect(10, 62, 16, 16, icon_color);
	draw_line(34, 62, label, COLOR_WHITE, 11);
}

/*
 * NEW in this lab -- an extra status line at the bottom of the screen
 * that the operator's UART "MODE" command turns on/off. Mode 0 (the
 * default, same as every earlier lab's screen) leaves it blank; mode 1
 * shows the latest message sequence number and M55's own uptime, a
 * simple example of a UART command actually changing what's on screen.
 * Only redrawn when a new ENV message arrives (see task_entry()), so a
 * MODE command's effect shows up on the NEXT periodic/event update, not
 * instantly -- documented in this lab's doc as an accepted simplification.
 */
static void draw_extra_line(uint32_t seq)
{
	char line[24];

	if (atomic_test_bit(display_mode, 0)) {
		/* Cast to uint32_t (not %lld on int64_t) -- this curriculum's
		 * minimal libc printf backend isn't guaranteed to support
		 * 64-bit format specifiers, and uptime-in-seconds comfortably
		 * fits in 32 bits for this lab's purposes. */
		uint32_t uptime_s = (uint32_t)(k_uptime_get() / 1000);

		snprintf(line, sizeof(line), "SEQ:%u UP:%us", seq, uptime_s);
		draw_line(10, 180, line, COLOR_YELLOW, 18);
	} else {
		draw_line(10, 180, "", COLOR_YELLOW, 18);
	}
}

static void rx_cb(const struct device *dev, mbox_channel_id_t channel_id,
		   void *user_data, struct mbox_msg *data)
{
	struct ipc13_msg msg;

	ARG_UNUSED(dev);
	ARG_UNUSED(channel_id);
	ARG_UNUSED(user_data);

	if (data->size < sizeof(msg)) {
		return;
	}
	memcpy(&msg, data->data, sizeof(msg));

	/* ISR-safe: pure memcpy + k_msgq_put, no drawing, no mbox_send_dt()
	 * here -- Display_Task and Cmd_Task do the actual work. */
	if (msg.type == IPC13_MSG_ENV) {
		k_msgq_put(&env_msgq, &msg.env, K_NO_WAIT);
	} else if (msg.type == IPC13_MSG_CMD) {
		k_msgq_put(&cmd_msgq, &msg.cmd, K_NO_WAIT);
	}
	/* IPC13_MSG_RESP is never expected here -- M55 only sends it, never
	 * receives it -- and is ignored if it somehow arrived. */
}

static void task_entry(void *p1, void *p2, void *p3)
{
	struct ipc13_env_payload msg;
	char line[24];
	int64_t last_rx_uptime = 0;
	bool ever_connected = false;
	enum conn_state state = CONN_WAITING;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	LOG_INF("[Display_Task] started");

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
	st7789_draw_string(10, 10, "LAB13 BRIDGE", COLOR_YELLOW, COLOR_BLACK, 2);
	draw_status(CONN_WAITING);
	draw_sensor_status(0);
	draw_line(10, 90, "T:--", COLOR_WHITE, 12);
	draw_line(10, 120, "H:--", COLOR_WHITE, 12);
	draw_line(10, 150, "P:--", COLOR_WHITE, 12);
	draw_extra_line(0);

	while (1) {
		int ret = k_msgq_get(&env_msgq, &msg, K_MSEC(POLL_TIMEOUT_MS));

		if (ret == 0) {
			last_rx_uptime = k_uptime_get();
			ever_connected = true;
			if (state != CONN_OK) {
				state = CONN_OK;
				draw_status(state);
			}

			char temp_str[8], hum_str[8], press_str[8];
			bool have_aht20 = (msg.sensor_mask & IPC13_SENSOR_AHT20) != 0;
			bool have_bmp280 = (msg.sensor_mask & IPC13_SENSOR_BMP280) != 0;
			bool aht20_failing = (msg.error_mask & IPC13_ERROR_AHT20) != 0;
			bool bmp280_failing = (msg.error_mask & IPC13_ERROR_BMP280) != 0;

			draw_sensor_status(msg.error_mask);

			if (have_aht20 && !aht20_failing) {
				format_milli_1dp(msg.temp_milli_c, temp_str, sizeof(temp_str));
				snprintf(line, sizeof(line), "T:%sC", temp_str);
			} else if (have_bmp280 && !bmp280_failing) {
				format_milli_1dp(msg.temp_milli_c, temp_str, sizeof(temp_str));
				snprintf(line, sizeof(line), "T:%sC(bmp)", temp_str);
			} else if (have_aht20 || have_bmp280) {
				snprintf(line, sizeof(line), "T:ERR");
			} else {
				snprintf(line, sizeof(line), "T:N/A");
			}
			draw_line(10, 90, line, COLOR_RED, 12);

			if (have_aht20 && !aht20_failing) {
				format_milli_1dp(msg.humidity_milli_pct, hum_str, sizeof(hum_str));
				snprintf(line, sizeof(line), "H:%s%%", hum_str);
			} else if (have_aht20) {
				snprintf(line, sizeof(line), "H:ERR");
			} else {
				snprintf(line, sizeof(line), "H:N/A");
			}
			draw_line(10, 120, line, COLOR_GREEN, 12);

			if (have_bmp280 && !bmp280_failing) {
				format_milli_1dp(msg.pressure_milli_hpa, press_str, sizeof(press_str));
				snprintf(line, sizeof(line), "P:%shPa", press_str);
			} else if (have_bmp280) {
				snprintf(line, sizeof(line), "P:ERR");
			} else {
				snprintf(line, sizeof(line), "P:N/A");
			}
			draw_line(10, 150, line, COLOR_CYAN, 12);

			draw_extra_line(msg.seq);

			k_mutex_lock(&latest_lock, K_FOREVER);
			latest_env = msg;
			latest_state = state;
			k_mutex_unlock(&latest_lock);

			LOG_INF("[Display_Task] seq=%u reason=%u mask=0x%x err=0x%x temp=%d hum=%d press=%d",
				msg.seq, msg.reason, msg.sensor_mask, msg.error_mask,
				msg.temp_milli_c, msg.humidity_milli_pct, msg.pressure_milli_hpa);
		} else if (ever_connected && state != CONN_LOST &&
			   (k_uptime_get() - last_rx_uptime) >= CONN_TIMEOUT_MS) {
			state = CONN_LOST;
			draw_status(state);
			k_mutex_lock(&latest_lock, K_FOREVER);
			latest_state = state;
			k_mutex_unlock(&latest_lock);
			LOG_WRN("[Display_Task] no message from M4 in %d ms - marked disconnected",
				CONN_TIMEOUT_MS);
		}
	}
}

static void cmd_task_entry(void *p1, void *p2, void *p3)
{
	struct ipc13_cmd_payload cmd;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	LOG_INF("[Cmd_Task] started");

	while (1) {
		if (k_msgq_get(&cmd_msgq, &cmd, K_FOREVER) != 0) {
			continue;
		}

		struct ipc13_resp_payload resp = {.cmd = cmd.cmd, .seq = cmd.seq};

		switch (cmd.cmd) {
		case IPC13_CMD_PING:
			resp.ok = 1;
			snprintf(resp.text, sizeof(resp.text), "PONG");
			break;

		case IPC13_CMD_STATUS: {
			struct ipc13_env_payload env;
			enum conn_state state;
			char temp_str[8], hum_str[8], press_str[8];
			static const char *const state_str[] = {"WAIT", "OK", "LOST"};

			k_mutex_lock(&latest_lock, K_FOREVER);
			env = latest_env;
			state = latest_state;
			k_mutex_unlock(&latest_lock);

			format_milli_1dp(env.temp_milli_c, temp_str, sizeof(temp_str));
			format_milli_1dp(env.humidity_milli_pct, hum_str, sizeof(hum_str));
			format_milli_1dp(env.pressure_milli_hpa, press_str, sizeof(press_str));

			resp.ok = 1;
			snprintf(resp.text, sizeof(resp.text), "T:%sC H:%s%% P:%shPa CONN:%s ERR:0x%x",
				 temp_str, hum_str, press_str, state_str[state], env.error_mask);
			break;
		}

		case IPC13_CMD_MODE:
			if (cmd.arg == 0 || cmd.arg == 1) {
				atomic_set_bit_to(display_mode, 0, cmd.arg == 1);
				resp.ok = 1;
				snprintf(resp.text, sizeof(resp.text), "MODE %u OK", cmd.arg);
			} else {
				resp.ok = 0;
				snprintf(resp.text, sizeof(resp.text), "MODE must be 0 or 1");
			}
			break;

		default:
			resp.ok = 0;
			snprintf(resp.text, sizeof(resp.text), "internal error: bad cmd %u", cmd.cmd);
			break;
		}

		struct ipc13_msg msg = {.type = IPC13_MSG_RESP, .resp = resp};
		struct mbox_msg mbox_msg = {.data = &msg, .size = sizeof(msg)};

		mbox_send_dt(&tx_channel, &mbox_msg);
		LOG_INF("[Cmd_Task] cmd=%u arg=%u seq=%u -> ok=%u \"%s\"",
			cmd.cmd, cmd.arg, cmd.seq, resp.ok, resp.text);
	}
}

int main(void)
{
	struct mbox_dt_spec rx_channel;

	LOG_INF("Lab13 UART Bridge HOST - %s", CONFIG_BOARD_TARGET);

	k_mutex_init(&latest_lock);

	tx_channel = (struct mbox_dt_spec)MBOX_DT_SPEC_GET(DT_PATH(mbox_consumer), tx);
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
			task_entry, NULL, NULL, NULL, TASK_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&task_data, "Display_Task");

	k_thread_create(&cmd_task_data, cmd_task_stack, K_THREAD_STACK_SIZEOF(cmd_task_stack),
			cmd_task_entry, NULL, NULL, NULL, CMD_TASK_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&cmd_task_data, "Cmd_Task");

	while (1) {
		k_sleep(K_FOREVER);
	}
	return 0;
}

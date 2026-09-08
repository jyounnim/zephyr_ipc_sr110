/*
 * Lab 12: AHT20 + BMP280 Environment Logging (HOST, M55)
 *
 * Receives temperature/humidity/pressure telemetry from M4 (the regular
 * 1s averages, the immediate out-of-band jump events, and now the
 * immediate sensor-read-error events) and renders them on the same
 * ST7789V3 TFT hardware setup as Lab 11 (identical panel, identical
 * SPI0/RESET/DC wiring, identical raw-SPI driver -- see this lab's
 * doc/12_aht20_bmp280_logging_kr.md for the full pin table and
 * level-shifter rationale, reproduced there rather than only in Lab 11's
 * doc, since each lab in this curriculum is meant to be buildable and
 * understandable on its own).
 *
 * Two INDEPENDENT fault indicators are shown, because they are two
 * independent failure modes on the M4 side:
 *   1. Connection status (top): did a message of ANY kind arrive from M4
 *      in the last 2 seconds? If not, the mbox/IPC link itself (or the
 *      whole M4 core) is assumed down. This does NOT necessarily mean a
 *      sensor is broken -- M4 could be alive but stuck.
 *   2. Sensor status (just below it): per the LATEST message's
 *      `error_mask`, did AHT20 and/or BMP280 fail their most recent I2C
 *      read? This can be true even while the connection is perfectly
 *      healthy -- M4 is alive and talking, but one of its sensors isn't
 *      answering (loose wire, sensor dropped off the bus, etc).
 * Mixing these two into a single indicator would hide which of "M4 died"
 * vs "a sensor died but M4 is fine" actually happened, so they're kept
 * as two separate lines on screen.
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
#include <stdlib.h>
#include "ipc_common.h"

LOG_MODULE_REGISTER(lab12_host, CONFIG_LOG_DEFAULT_LEVEL);

#define TASK_STACK_SIZE 2048
#define TASK_PRIORITY   4
#define POLL_TIMEOUT_MS 500   /* how often Display_Task checks for a connection timeout */
#define CONN_TIMEOUT_MS 2000  /* no message from M4 for this long -> "disconnected" */

K_THREAD_STACK_DEFINE(task_stack, TASK_STACK_SIZE);
static struct k_thread task_data;
K_MSGQ_DEFINE(env_msgq, sizeof(struct ipc12_env_msg), 4, 4);

#define DISP_NODE DT_NODELABEL(st7789v_disp)

#define PANEL_WIDTH   DT_PROP(DISP_NODE, width)
#define PANEL_HEIGHT  DT_PROP(DISP_NODE, height)
#define X_OFFSET      DT_PROP(DISP_NODE, x_offset)
#define Y_OFFSET      DT_PROP(DISP_NODE, y_offset)

/* ST7789 command set (same subset as Lab 11). */
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

/* RGB565 colors. */
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

/*
 * 5x7 font table. The entries carried over from Lab 11 are marked
 * "(Lab 11)"; everything else added for this lab's unit labels
 * ("T:23.5C", "H:45.2%", "P:1013.2hPa", "OK"/"LOST") is NEW and, like
 * Lab 11's additions, taken from the classic public-domain 5x7
 * "glcdfont" table without individual re-verification against this
 * panel -- check here first if a character looks wrong.
 */
struct glyph {
	char ch;
	uint8_t cols[5];
};

static const struct glyph font5x7[] = {
	{' ', {0x00, 0x00, 0x00, 0x00, 0x00}}, /* (Lab 11) */
	{'-', {0x08, 0x08, 0x08, 0x08, 0x08}}, /* (Lab 11) */
	{':', {0x00, 0x36, 0x36, 0x00, 0x00}}, /* (Lab 11) */
	{'.', {0x00, 0x60, 0x60, 0x00, 0x00}},
	{'%', {0x23, 0x13, 0x08, 0x64, 0x62}},
	{'0', {0x3E, 0x51, 0x49, 0x45, 0x3E}}, /* (Lab 11) */
	{'1', {0x00, 0x42, 0x7F, 0x40, 0x00}}, /* (Lab 11) */
	{'2', {0x62, 0x51, 0x49, 0x49, 0x46}}, /* (Lab 11) */
	{'3', {0x22, 0x41, 0x49, 0x49, 0x36}}, /* (Lab 11) */
	{'4', {0x18, 0x14, 0x12, 0x7F, 0x10}}, /* (Lab 11) */
	{'5', {0x27, 0x45, 0x45, 0x45, 0x39}}, /* (Lab 11) */
	{'6', {0x3C, 0x4A, 0x49, 0x49, 0x30}}, /* (Lab 11) */
	{'7', {0x01, 0x71, 0x09, 0x05, 0x03}}, /* (Lab 11) */
	{'8', {0x36, 0x49, 0x49, 0x49, 0x36}}, /* (Lab 11) */
	{'9', {0x06, 0x49, 0x49, 0x29, 0x1E}}, /* (Lab 11) */
	{'A', {0x7E, 0x11, 0x11, 0x11, 0x7E}}, /* (Lab 11) */
	{'B', {0x7F, 0x49, 0x49, 0x49, 0x36}}, /* (Lab 11) */
	{'C', {0x3E, 0x41, 0x41, 0x41, 0x22}},
	{'D', {0x7F, 0x41, 0x41, 0x22, 0x1C}},
	{'E', {0x7F, 0x49, 0x49, 0x49, 0x41}}, /* (Lab 11) */
	{'H', {0x7F, 0x08, 0x08, 0x08, 0x7F}},
	{'I', {0x00, 0x41, 0x7F, 0x41, 0x00}},
	{'K', {0x7F, 0x08, 0x14, 0x22, 0x41}},
	{'L', {0x7F, 0x40, 0x40, 0x40, 0x40}}, /* (Lab 11) */
	{'N', {0x7F, 0x02, 0x04, 0x08, 0x7F}},
	{'O', {0x3E, 0x41, 0x41, 0x41, 0x3E}},
	{'P', {0x7F, 0x09, 0x09, 0x09, 0x06}},
	{'Q', {0x3E, 0x41, 0x51, 0x21, 0x5E}}, /* (Lab 11) */
	{'R', {0x7F, 0x09, 0x19, 0x29, 0x46}}, /* (Lab 11) */
	{'S', {0x46, 0x49, 0x49, 0x49, 0x31}}, /* (Lab 11) */
	{'T', {0x01, 0x01, 0x7F, 0x01, 0x01}},
	{'V', {0x1F, 0x20, 0x40, 0x20, 0x1F}}, /* (Lab 11) */
	{'X', {0x63, 0x14, 0x08, 0x14, 0x63}}, /* (Lab 11) */
	{'Y', {0x07, 0x08, 0x70, 0x08, 0x07}}, /* (Lab 11) */
	{'Z', {0x61, 0x51, 0x49, 0x45, 0x43}}, /* (Lab 11) */
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

/* SR110 SPI0's 8-byte hardware FIFO limit -- see Lab 11 for the full
 * explanation. Every send chunks to this size. */
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

/* Prints one line padded with spaces to a fixed width, so a shorter new
 * value fully overwrites a longer old one without a separate clear. */
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

/* Formats a milli-unit fixed-point value as "[-]W.F" (one decimal digit)
 * into `out` -- avoids depending on floating-point printf support. */
static void format_milli_1dp(int32_t milli, char *out, size_t outsz)
{
	bool neg = milli < 0;
	int32_t m = neg ? -milli : milli;
	int32_t whole = m / 1000;
	int32_t frac = (m % 1000) / 100;

	snprintf(out, outsz, "%s%d.%d", neg ? "-" : "", whole, frac);
}

enum conn_state { CONN_WAITING, CONN_OK, CONN_LOST };

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

	/* Simple filled-square "icon" (this font/driver has no circle
	 * primitive) plus a short text label -- covers both the "icon if
	 * possible" and "text otherwise" request at once. */
	st7789_fill_rect(10, 40, 16, 16, icon_color);
	draw_line(34, 40, label, COLOR_WHITE, 6);
}

/*
 * Separate from draw_status() above on purpose -- see the file header
 * comment for why connection health and sensor health are shown as two
 * independent indicators instead of one. error_mask bits come straight
 * from the M4-side IPC12_ERROR_* flags: bit0 = AHT20 failing its most
 * recent read, bit1 = BMP280 failing its most recent read.
 */
static void draw_sensor_status(uint8_t error_mask)
{
	uint16_t icon_color;
	const char *label;

	if (!error_mask) {
		icon_color = COLOR_GREEN;
		label = "SENSOR OK";
	} else if ((error_mask & (IPC12_ERROR_AHT20 | IPC12_ERROR_BMP280)) ==
		   (IPC12_ERROR_AHT20 | IPC12_ERROR_BMP280)) {
		icon_color = COLOR_RED;
		label = "SENSOR ERR"; /* both sensors failing */
	} else if (error_mask & IPC12_ERROR_AHT20) {
		icon_color = COLOR_RED;
		label = "AHT20 ERR";
	} else {
		icon_color = COLOR_RED;
		label = "BMP280 ERR";
	}

	st7789_fill_rect(10, 62, 16, 16, icon_color);
	draw_line(34, 62, label, COLOR_WHITE, 11);
}

static void rx_cb(const struct device *dev, mbox_channel_id_t channel_id,
		   void *user_data, struct mbox_msg *data)
{
	struct ipc12_env_msg msg;

	ARG_UNUSED(dev);
	ARG_UNUSED(channel_id);
	ARG_UNUSED(user_data);

	if (data->size < sizeof(msg)) {
		return;
	}
	memcpy(&msg, data->data, sizeof(msg));
	/* SPI drawing blocks -- ISR only enqueues, Display_Task does the work. */
	k_msgq_put(&env_msgq, &msg, K_NO_WAIT);
}

static void task_entry(void *p1, void *p2, void *p3)
{
	struct ipc12_env_msg msg;
	char line[24];
	int64_t last_rx_uptime = 0;
	bool ever_connected = false;
	enum conn_state state = CONN_WAITING;

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
	st7789_draw_string(10, 10, "LAB12 ENV", COLOR_YELLOW, COLOR_BLACK, 2);
	draw_status(CONN_WAITING);
	draw_sensor_status(0);
	draw_line(10, 90, "T:--", COLOR_WHITE, 12);
	draw_line(10, 120, "H:--", COLOR_WHITE, 12);
	draw_line(10, 150, "P:--", COLOR_WHITE, 12);

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
			bool have_aht20 = (msg.sensor_mask & IPC12_SENSOR_AHT20) != 0;
			bool have_bmp280 = (msg.sensor_mask & IPC12_SENSOR_BMP280) != 0;
			bool aht20_failing = (msg.error_mask & IPC12_ERROR_AHT20) != 0;
			bool bmp280_failing = (msg.error_mask & IPC12_ERROR_BMP280) != 0;

			draw_sensor_status(msg.error_mask);

			/* Temperature: AHT20 is the primary source. Fall back to
			 * BMP280's own temperature reading whenever AHT20 wasn't
			 * found at boot at all, OR it was found but is currently
			 * failing to read (error_mask) -- in both cases the
			 * `temp_milli_c` field already carries BMP280's value,
			 * see M4's main.c. Only if BOTH possible sources are
			 * unavailable/failing do we show an outright error. */
			if (have_aht20 && !aht20_failing) {
				format_milli_1dp(msg.temp_milli_c, temp_str, sizeof(temp_str));
				snprintf(line, sizeof(line), "T:%sC", temp_str);
			} else if (have_bmp280 && !bmp280_failing) {
				format_milli_1dp(msg.temp_milli_c, temp_str, sizeof(temp_str));
				snprintf(line, sizeof(line), "T:%sC(bmp)", temp_str);
			} else if (have_aht20 || have_bmp280) {
				/* Detected at boot, but every temperature source is
				 * currently failing to read. */
				snprintf(line, sizeof(line), "T:ERR");
			} else {
				snprintf(line, sizeof(line), "T:N/A");
			}
			draw_line(10, 90, line, COLOR_RED, 12);

			/* Humidity has no fallback sensor -- AHT20 or nothing. */
			if (have_aht20 && !aht20_failing) {
				format_milli_1dp(msg.humidity_milli_pct, hum_str, sizeof(hum_str));
				snprintf(line, sizeof(line), "H:%s%%", hum_str);
			} else if (have_aht20) {
				snprintf(line, sizeof(line), "H:ERR");
			} else {
				snprintf(line, sizeof(line), "H:N/A");
			}
			draw_line(10, 120, line, COLOR_GREEN, 12);

			/* Pressure has no fallback sensor -- BMP280 or nothing. */
			if (have_bmp280 && !bmp280_failing) {
				format_milli_1dp(msg.pressure_milli_hpa, press_str, sizeof(press_str));
				snprintf(line, sizeof(line), "P:%shPa", press_str);
			} else if (have_bmp280) {
				snprintf(line, sizeof(line), "P:ERR");
			} else {
				snprintf(line, sizeof(line), "P:N/A");
			}
			draw_line(10, 150, line, COLOR_CYAN, 12);

			LOG_INF("[Display_Task] seq=%u reason=%u mask=0x%x err=0x%x temp=%d hum=%d press=%d",
				msg.seq, msg.reason, msg.sensor_mask, msg.error_mask,
				msg.temp_milli_c, msg.humidity_milli_pct, msg.pressure_milli_hpa);
		} else if (ever_connected && state != CONN_LOST &&
			   (k_uptime_get() - last_rx_uptime) >= CONN_TIMEOUT_MS) {
			state = CONN_LOST;
			draw_status(state);
			LOG_WRN("[Display_Task] no message from M4 in %d ms - marked disconnected",
				CONN_TIMEOUT_MS);
		}
	}
}

int main(void)
{
	struct mbox_dt_spec rx_channel;

	LOG_INF("Lab12 Env Logging HOST - %s", CONFIG_BOARD_TARGET);

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

	while (1) {
		k_sleep(K_FOREVER);
	}
	return 0;
}

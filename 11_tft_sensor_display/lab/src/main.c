/*
 * Lab 11: TFT Sensor Display (HOST, M55)
 * Receives the X/Y/Z accelerometer values M4 samples from the onboard
 * MC3419 and renders them as text on an ST7789V3 1.69" 240x280 SPI TFT.
 *
 * The SPI0/driver/panel-geometry portion of this file is ported
 * unchanged from jyounnim/zephyr_display-sr110's Lab 06
 * (06_TFT_ST7789V3), which was already hardware-verified standalone on
 * this exact board (level shifter requirement, 8-byte SPI0 FIFO chunking,
 * full init sequence, panel RAM offset -- see that lab's doc/
 * troubleshooting notes for the full bring-up story). It talks to the
 * panel directly with raw spi_write_dt() calls rather than Zephyr's
 * in-tree Display/CFB subsystem -- see the "zds,st7789v" custom binding
 * in dts/bindings/display/.
 *
 * What's new in THIS file (on top of that confirmed base): the mbox
 * ISR -> k_msgq -> worker-thread pattern used by every lab in this IPC
 * curriculum (Display_Task is the worker), and a small font-table
 * extension (digits, ':', '-', and the specific uppercase letters used
 * below) so accelerometer values can be printed -- the original lab's
 * font5x7 table only covered the characters needed for "Hello World!".
 *
 * M55 drives this display over SPI0, which physically shares pins with
 * M55's own console UART (see the M55 overlay for the full explanation).
 * That means this build can't rely on its serial console for status info
 * -- the TFT itself is the only debug/status output available once this
 * overlay is flashed, so this main() puts a small status line (rx count)
 * on screen in addition to the sensor values.
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

LOG_MODULE_REGISTER(lab11_host, CONFIG_LOG_DEFAULT_LEVEL);

#define TASK_STACK_SIZE 2048
#define TASK_PRIORITY   4

K_THREAD_STACK_DEFINE(task_stack, TASK_STACK_SIZE);
static struct k_thread task_data;
K_MSGQ_DEFINE(accel_msgq, sizeof(struct ipc11_accel_msg), 4, 4);

#define DISP_NODE DT_NODELABEL(st7789v_disp)

#define PANEL_WIDTH   DT_PROP(DISP_NODE, width)
#define PANEL_HEIGHT  DT_PROP(DISP_NODE, height)
#define X_OFFSET      DT_PROP(DISP_NODE, x_offset)
#define Y_OFFSET      DT_PROP(DISP_NODE, y_offset)

/* ST7789 command set (only what this lab needs). */
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

static const struct spi_dt_spec spi_spec =
	SPI_DT_SPEC_GET(DISP_NODE, SPI_WORD_SET(8) | SPI_TRANSFER_MSB, 0);
static const struct gpio_dt_spec reset_spec = GPIO_DT_SPEC_GET(DISP_NODE, reset_gpios);
static const struct gpio_dt_spec dc_spec = GPIO_DT_SPEC_GET(DISP_NODE, dc_gpios);

/*
 * 5x7 font table. The entries marked "(from Lab 06)" are copied verbatim
 * from the confirmed zephyr_display-sr110 source. Everything else (digits,
 * ':', '-', and the extra uppercase letters used in this lab's on-screen
 * labels) is a NEW addition, taken from the classic public-domain 5x7
 * "glcdfont" table used throughout the Arduino/Adafruit-GFX ecosystem --
 * it has not been individually re-verified against this specific panel,
 * so if a character looks wrong on real hardware, that's the first place
 * to check.
 */
struct glyph {
	char ch;
	uint8_t cols[5];
};

static const struct glyph font5x7[] = {
	{' ', {0x00, 0x00, 0x00, 0x00, 0x00}}, /* (from Lab 06) */
	{'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
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
	{'E', {0x7F, 0x49, 0x49, 0x49, 0x41}},
	{'L', {0x7F, 0x40, 0x40, 0x40, 0x40}},
	{'Q', {0x3E, 0x41, 0x51, 0x21, 0x5E}},
	{'R', {0x7F, 0x09, 0x19, 0x29, 0x46}},
	{'S', {0x46, 0x49, 0x49, 0x49, 0x31}},
	{'V', {0x1F, 0x20, 0x40, 0x20, 0x1F}},
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

/* SR110: SPI0's hardware FIFO is only 8 bytes deep (fifo-depth = <8>; in
 * sr100_m55.dtsi). A single spi_write_dt() call needing a mid-transfer
 * TX-FIFO-refill interrupt to complete (any single call over 8 bytes)
 * reproducibly times out (-ETIMEDOUT) instead of that refill interrupt
 * ever firing -- confirmed in Lab 06's bring-up. Every send below is
 * chunked to <= 8 bytes to avoid ever needing that refill.
 */
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
	gpio_pin_set_dt(&reset_spec, 1); /* assert reset */
	k_sleep(K_MSEC(10));
	gpio_pin_set_dt(&reset_spec, 0); /* release reset */
	k_sleep(K_MSEC(150));            /* datasheet: allow up to ~120ms to recover */

	return 0;
}

/* Full power/frame-rate/gamma init sequence (not the bare MIPI-DCS
 * minimum) -- confirmed necessary on this exact panel/SPI0 setup in
 * Lab 06's bring-up (the bare minimum completed with no SPI error but
 * never produced a visible image). Cross-checked there against ESPHome's
 * st7789v component, Bodmer's TFT_eSPI library, and Adafruit's Raspberry
 * Pi fbtft driver, which all agree on this sequence/these values.
 */
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
	uint8_t row[5 * 8 * 2]; /* generous fixed max: 5 cols * up to 8x scale * 2 bytes */
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
		x += 6 * scale; /* 5 glyph columns + 1 blank column */
	}
	return 0;
}

/* Prints one line, padded with spaces to `width` characters, so a shorter
 * new value fully overwrites a longer old one without needing a full
 * fill_rect() clear first (cheaper -- avoids re-sending the whole row). */
static void draw_line(uint16_t y, const char *text, uint16_t fg)
{
	char padded[24];
	size_t len = strlen(text);

	if (len >= sizeof(padded)) {
		len = sizeof(padded) - 1;
	}
	memcpy(padded, text, len);
	for (size_t i = len; i < sizeof(padded) - 1; i++) {
		padded[i] = ' ';
	}
	padded[sizeof(padded) - 1] = '\0';

	st7789_draw_string(10, y, padded, fg, COLOR_BLACK, 2);
}

static void rx_cb(const struct device *dev, mbox_channel_id_t channel_id,
		   void *user_data, struct mbox_msg *data)
{
	struct ipc11_accel_msg msg;

	ARG_UNUSED(dev);
	ARG_UNUSED(channel_id);
	ARG_UNUSED(user_data);

	if (data->size < sizeof(msg)) {
		return;
	}
	memcpy(&msg, data->data, sizeof(msg));
	/* Driving the TFT (raw spi_write_dt() calls) blocks, so this ISR-safe
	 * callback only enqueues the message; Display_Task does the real work
	 * (same ISR -> k_msgq -> worker-thread pattern as every other lab). */
	k_msgq_put(&accel_msgq, &msg, K_NO_WAIT);
}

static void task_entry(void *p1, void *p2, void *p3)
{
	struct ipc11_accel_msg msg;
	char line[24];
	uint32_t rx_count = 0;

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
	st7789_draw_string(10, 10, "LAB11 ACCEL", COLOR_YELLOW, COLOR_BLACK, 2);

	while (1) {
		if (k_msgq_get(&accel_msgq, &msg, K_FOREVER) != 0) {
			continue;
		}
		rx_count++;

		snprintf(line, sizeof(line), "SEQ:%u RX:%u", msg.seq, rx_count);
		draw_line(40, line, COLOR_WHITE);

		snprintf(line, sizeof(line), "X:%d", msg.x);
		draw_line(70, line, COLOR_RED);

		snprintf(line, sizeof(line), "Y:%d", msg.y);
		draw_line(100, line, COLOR_GREEN);

		snprintf(line, sizeof(line), "Z:%d", msg.z);
		draw_line(130, line, COLOR_YELLOW);

		LOG_INF("[Display_Task] seq=%u x=%d y=%d z=%d", msg.seq, msg.x, msg.y, msg.z);
	}
}

int main(void)
{
	struct mbox_dt_spec rx_channel;

	LOG_INF("Lab11 TFT Sensor Display HOST - %s", CONFIG_BOARD_TARGET);

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

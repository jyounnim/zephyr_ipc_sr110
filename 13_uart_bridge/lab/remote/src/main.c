/*
 * Lab 13: UART Bridge (CLIENT, M4)
 *
 * This lab adds an operator command channel on top of Lab 12's dual
 * AHT20+BMP280 environment logger, WITHOUT any new wiring: the AHT20 and
 * BMP280 sensors on &i2c0 keep being sampled and reported exactly as in
 * Lab 12, and the new command channel reuses M4's own console UART
 * (`ns16550_uart0`, already wired out to the J24 header for every lab's
 * boot log) as a bidirectional link, instead of requiring a second,
 * separate UART peripheral and a fresh set of wires.
 *
 * DESIGN CHOICE -- reuse the console UART as a command channel: every
 * lab in this curriculum already has a terminal open on M4's console to
 * read boot/debug logs (see the course overview's "M4 콘솔" connection
 * instructions -- J24, 230400bps). J24 pin 14 (M4 RX) has always been
 * physically wired, it just hasn't been used for anything yet, since no
 * lab so far reads operator input. This lab is the first to actually use
 * that RX line: an operator can type a short text command into the same
 * terminal window that shows the boot log, press Enter, and M4 relays it
 * to M55 over mbox.
 *
 * HOW THE UART SIDE WORKS (REVISED -- see note below): this lab originally
 * tried `CONFIG_UART_INTERRUPT_DRIVEN=y` with `uart_irq_callback_set()` /
 * `uart_irq_rx_enable()` to install an RX interrupt callback on the
 * console UART. On real hardware, `uart_irq_callback_set()` returned
 * -ENOTSUP (-134) for `ns16550_uart0` on this board -- i.e. this UART
 * instance's driver simply does not implement interrupt-driven RX, no
 * matter the Kconfig setting. This is a hardware/driver capability limit,
 * not a bug in this lab's code, so the design was changed to POLLING
 * instead: a dedicated low-priority thread (`Uart_Cmd_Task`) calls
 * `uart_poll_in()` in a loop with a short sleep between attempts. This is
 * slightly less responsive than an interrupt (bounded by the poll
 * interval, currently 10ms) but needs no special UART capability and
 * coexists cleanly with the normal printk/log console output already
 * using this same UART's TX side (`uart_poll_out()`, used internally by
 * printk, and our own `uart_poll_in()` are independent directions of the
 * same simple polling API).
 *
 * LESSON: always check the return value of a Zephyr driver API call
 * instead of assuming a Kconfig symbol guarantees hardware support --
 * `CONFIG_UART_INTERRUPT_DRIVEN=y` only compiles the interrupt-driven
 * code path in; whether a *specific* UART instance's driver actually
 * implements it is a separate, per-instance question answered by the
 * function's return code.
 *
 * MESSAGE MULTIPLEXING: this lab's mbox channel now carries three
 * different message shapes (periodic sensor telemetry, a command, and a
 * command response), tagged with `ipc13_msg.type` -- see ipc_common.h.
 * Because BOTH the sensor-sampling loop (in main()) and `Uart_Cmd_Task`
 * can call `mbox_send_dt()` on the same tx_channel from two different
 * threads, `tx_lock` serializes them so one thread's send can't be
 * interleaved with another's.
 *
 * The AHT20/BMP280 driver code below (probe/init/read, error tracking,
 * active recovery) is IDENTICAL to Lab 12's -- see that lab's file header
 * comment and doc for the full protocol/design rationale, reproduced
 * here only where this lab's own behavior differs.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "ipc_common.h"

LOG_MODULE_REGISTER(lab13_client, CONFIG_LOG_DEFAULT_LEVEL);

#define SAMPLE_PERIOD_MS         200
#define SAMPLES_PER_AVERAGE      (1000 / SAMPLE_PERIOD_MS) /* = 5 */
#define TEMP_JUMP_MILLI_C        500     /* 0.5 C */
#define PRESSURE_JUMP_MILLI_HPA  50000   /* 50 hPa */
#define FAIL_RECOVER_THRESHOLD   5

static const struct device *i2c_bus = DEVICE_DT_GET(DT_NODELABEL(i2c0));
static struct mbox_dt_spec tx_channel;
static struct k_mutex tx_lock;

/* ---------------------------------------------------------------- AHT20 */

#define AHT20_ADDR            0x38
#define AHT20_CMD_SOFT_RESET  0xBA
#define AHT20_CMD_INIT        0xBE
#define AHT20_CMD_TRIGGER     0xAC
#define AHT20_STATUS_BUSY_BIT 0x80

static bool aht20_probe(void)
{
	uint8_t dummy;

	return i2c_read(i2c_bus, &dummy, 1, AHT20_ADDR) == 0;
}

static int aht20_init(void)
{
	uint8_t reset_cmd = AHT20_CMD_SOFT_RESET;
	uint8_t init_cmd[3] = {AHT20_CMD_INIT, 0x08, 0x00};
	int ret;

	ret = i2c_write(i2c_bus, &reset_cmd, 1, AHT20_ADDR);
	if (ret) return ret;
	k_msleep(20);

	ret = i2c_write(i2c_bus, init_cmd, sizeof(init_cmd), AHT20_ADDR);
	k_msleep(10);
	return ret;
}

static int aht20_read(int32_t *temp_milli_c, int32_t *humidity_milli_pct)
{
	uint8_t trigger_cmd[3] = {AHT20_CMD_TRIGGER, 0x33, 0x00};
	uint8_t data[6];
	int ret;

	ret = i2c_write(i2c_bus, trigger_cmd, sizeof(trigger_cmd), AHT20_ADDR);
	if (ret) return ret;
	k_msleep(80);

	ret = i2c_read(i2c_bus, data, sizeof(data), AHT20_ADDR);
	if (ret) return ret;

	if (data[0] & AHT20_STATUS_BUSY_BIT) {
		return -EBUSY;
	}

	uint32_t raw_hum = ((uint32_t)data[1] << 12) | ((uint32_t)data[2] << 4) | (data[3] >> 4);
	uint32_t raw_temp = (((uint32_t)data[3] & 0x0F) << 16) | ((uint32_t)data[4] << 8) | data[5];

	*humidity_milli_pct = (int32_t)(((uint64_t)raw_hum * 100000ULL) >> 20);
	*temp_milli_c = (int32_t)(((uint64_t)raw_temp * 200000ULL) >> 20) - 50000;

	return 0;
}

/* --------------------------------------------------------------- BMP280 */

#define BMP280_REG_CHIPID        0xD0
#define BMP280_REG_CTRL_MEAS     0xF4
#define BMP280_REG_CONFIG        0xF5
#define BMP280_REG_CALIB_START   0x88
#define BMP280_REG_PRESS_MSB     0xF7
#define BMP280_CHIPID_BMP280     0x58
#define BMP280_CHIPID_BME280     0x60

struct bmp280_calib {
	uint16_t dig_T1;
	int16_t  dig_T2, dig_T3;
	uint16_t dig_P1;
	int16_t  dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;
};

static uint8_t bmp280_addr;
static struct bmp280_calib bmp280_calib;

static bool bmp280_probe(uint8_t addr)
{
	uint8_t reg = BMP280_REG_CHIPID;
	uint8_t id;

	if (i2c_write_read(i2c_bus, addr, &reg, 1, &id, 1) != 0) {
		return false;
	}
	return id == BMP280_CHIPID_BMP280 || id == BMP280_CHIPID_BME280;
}

static int bmp280_read_calib(void)
{
	uint8_t reg = BMP280_REG_CALIB_START;
	uint8_t buf[24];
	int ret;

	ret = i2c_write_read(i2c_bus, bmp280_addr, &reg, 1, buf, sizeof(buf));
	if (ret) return ret;

	bmp280_calib.dig_T1 = sys_get_le16(&buf[0]);
	bmp280_calib.dig_T2 = (int16_t)sys_get_le16(&buf[2]);
	bmp280_calib.dig_T3 = (int16_t)sys_get_le16(&buf[4]);
	bmp280_calib.dig_P1 = sys_get_le16(&buf[6]);
	bmp280_calib.dig_P2 = (int16_t)sys_get_le16(&buf[8]);
	bmp280_calib.dig_P3 = (int16_t)sys_get_le16(&buf[10]);
	bmp280_calib.dig_P4 = (int16_t)sys_get_le16(&buf[12]);
	bmp280_calib.dig_P5 = (int16_t)sys_get_le16(&buf[14]);
	bmp280_calib.dig_P6 = (int16_t)sys_get_le16(&buf[16]);
	bmp280_calib.dig_P7 = (int16_t)sys_get_le16(&buf[18]);
	bmp280_calib.dig_P8 = (int16_t)sys_get_le16(&buf[20]);
	bmp280_calib.dig_P9 = (int16_t)sys_get_le16(&buf[22]);
	return 0;
}

static int bmp280_configure(void)
{
	uint8_t ctrl_meas[2] = {BMP280_REG_CTRL_MEAS, 0x27};
	uint8_t config[2] = {BMP280_REG_CONFIG, 0x00};
	int ret;

	ret = i2c_write(i2c_bus, ctrl_meas, sizeof(ctrl_meas), bmp280_addr);
	if (ret) return ret;
	return i2c_write(i2c_bus, config, sizeof(config), bmp280_addr);
}

static int32_t bmp280_compensate_temp(int32_t adc_T, int32_t *t_fine)
{
	int32_t var1, var2;

	var1 = ((((adc_T >> 3) - ((int32_t)bmp280_calib.dig_T1 << 1))) *
		((int32_t)bmp280_calib.dig_T2)) >> 11;
	var2 = (((((adc_T >> 4) - ((int32_t)bmp280_calib.dig_T1)) *
		  ((adc_T >> 4) - ((int32_t)bmp280_calib.dig_T1))) >> 12) *
		((int32_t)bmp280_calib.dig_T3)) >> 14;
	*t_fine = var1 + var2;
	return (*t_fine * 5 + 128) >> 8;
}

static uint32_t bmp280_compensate_pressure(int32_t adc_P, int32_t t_fine)
{
	int64_t var1, var2, p;

	var1 = (int64_t)t_fine - 128000;
	var2 = var1 * var1 * (int64_t)bmp280_calib.dig_P6;
	var2 = var2 + ((var1 * (int64_t)bmp280_calib.dig_P5) << 17);
	var2 = var2 + (((int64_t)bmp280_calib.dig_P4) << 35);
	var1 = ((var1 * var1 * (int64_t)bmp280_calib.dig_P3) >> 8) +
	       ((var1 * (int64_t)bmp280_calib.dig_P2) << 12);
	var1 = ((((int64_t)1) << 47) + var1) * ((int64_t)bmp280_calib.dig_P1) >> 33;
	if (var1 == 0) {
		return 0;
	}
	p = 1048576 - adc_P;
	p = (((p << 31) - var2) * 3125) / var1;
	var1 = (((int64_t)bmp280_calib.dig_P9) * (p >> 13) * (p >> 13)) >> 25;
	var2 = (((int64_t)bmp280_calib.dig_P8) * p) >> 19;
	p = ((p + var1 + var2) >> 8) + (((int64_t)bmp280_calib.dig_P7) << 4);
	return (uint32_t)p;
}

static int bmp280_read(int32_t *temp_milli_c, int32_t *pressure_milli_hpa)
{
	uint8_t reg = BMP280_REG_PRESS_MSB;
	uint8_t buf[6];
	int32_t adc_P, adc_T, t_fine;
	int ret;

	ret = i2c_write_read(i2c_bus, bmp280_addr, &reg, 1, buf, sizeof(buf));
	if (ret) return ret;

	adc_P = ((int32_t)buf[0] << 12) | ((int32_t)buf[1] << 4) | (buf[2] >> 4);
	adc_T = ((int32_t)buf[3] << 12) | ((int32_t)buf[4] << 4) | (buf[5] >> 4);

	int32_t temp_centi_c = bmp280_compensate_temp(adc_T, &t_fine);
	uint32_t pressure_q24_8 = bmp280_compensate_pressure(adc_P, t_fine);

	*temp_milli_c = temp_centi_c * 10;
	*pressure_milli_hpa = (int32_t)(((int64_t)pressure_q24_8 * 10) / 256);

	return 0;
}

/* ------------------------------------------------------- sensor -> mbox */

static void send_env(int32_t temp, int32_t hum, int32_t press, uint8_t mask,
		      uint8_t error_mask, enum ipc13_env_reason reason, uint32_t seq)
{
	struct ipc13_msg msg = {
		.type = IPC13_MSG_ENV,
		.env = {
			.temp_milli_c = temp,
			.humidity_milli_pct = hum,
			.pressure_milli_hpa = press,
			.seq = seq,
			.sensor_mask = mask,
			.error_mask = error_mask,
			.reason = reason,
		},
	};
	struct mbox_msg mbox_msg = {.data = &msg, .size = sizeof(msg)};

	k_mutex_lock(&tx_lock, K_FOREVER);
	mbox_send_dt(&tx_channel, &mbox_msg);
	k_mutex_unlock(&tx_lock);
}

/* ------------------------------------------------- UART command intake */

#define UART_TASK_STACK_SIZE 1536
#define UART_TASK_PRIORITY   5
#define RESP_TASK_STACK_SIZE 1024
#define RESP_TASK_PRIORITY   5

K_THREAD_STACK_DEFINE(uart_task_stack, UART_TASK_STACK_SIZE);
static struct k_thread uart_task_data;
K_THREAD_STACK_DEFINE(resp_task_stack, RESP_TASK_STACK_SIZE);
static struct k_thread resp_task_data;

K_MSGQ_DEFINE(resp_msgq, sizeof(struct ipc13_resp_payload), 4, 4);

static const struct device *console_dev = DEVICE_DT_GET(DT_NODELABEL(ns16550_uart0));

/* Poll interval for Uart_Cmd_Task's uart_poll_in() loop -- see the file
 * header comment for why polling is used instead of an RX interrupt.
 * 10ms is short enough that a human typing feels instant, and long
 * enough not to waste CPU spinning on an idle UART. */
#define UART_POLL_INTERVAL_MS 10

/* Controls whether the "periodic avg ..." LOG_INF line in main()'s sensor
 * loop prints. Defaults to on (matches every earlier lab's behavior).
 * This is an M4-LOCAL setting only -- unlike commands 1/2/3, command 4
 * never crosses the mbox channel to M55 at all, because it has nothing
 * to do with M55: it only silences M4's own console noise so an operator
 * testing commands 1/2/3 isn't fighting a periodic log line scrolling
 * the screen out from under them. This is a deliberate second example
 * (after the "reject invalid commands locally" choice) of a design
 * question every command needs answered: does this command actually
 * need to reach the other core, or can -- should -- it stay local? */
static atomic_t periodic_log_enabled = ATOMIC_INIT(1);

/* Parses one command line and, if valid, fills *out_cmd and returns true.
 * Runs in Uart_Cmd_Task's thread context.
 *
 * NOTE: this used to accept text commands ("PING", "STATUS", "MODE <0|1>").
 * Switched to single-digit numeric commands (1/2/3) so a test over a raw
 * terminal only needs one keystroke + Enter -- easier to rule out
 * terminal/line-ending issues while bringing this feature up on real
 * hardware than typing a multi-character word each time. */
static bool parse_command(const char *line, struct ipc13_cmd_payload *out_cmd)
{
	int arg;

	if (strcmp(line, "1") == 0) {
		out_cmd->cmd = IPC13_CMD_PING;
		out_cmd->arg = 0;
		return true;
	}
	if (strcmp(line, "2") == 0) {
		out_cmd->cmd = IPC13_CMD_STATUS;
		out_cmd->arg = 0;
		return true;
	}
	if (sscanf(line, "3 %d", &arg) == 1 && (arg == 0 || arg == 1)) {
		out_cmd->cmd = IPC13_CMD_MODE;
		out_cmd->arg = (uint8_t)arg;
		return true;
	}
	return false;
}

/* Handles a command that never needs to reach M55 -- currently just "4
 * <0|1>", the periodic-log on/off toggle (see periodic_log_enabled
 * above). Returns true if the line was recognized and handled here, in
 * which case the caller must NOT also try parse_command()/mbox_send_dt()
 * on it. Kept as a separate function (rather than folded into
 * parse_command()) specifically to make this M4-local-vs-M55-bound
 * distinction visible in the code, not just in a comment. */
static bool handle_local_command(const char *line)
{
	int arg;

	if (sscanf(line, "4 %d", &arg) == 1 && (arg == 0 || arg == 1)) {
		atomic_set(&periodic_log_enabled, arg);
		printk("[M4] periodic log %s (M4-local -- not sent to M55)\n",
		       arg ? "ON" : "OFF");
		return true;
	}
	return false;
}

/* Polls the console UART for one line at a time (see file header comment
 * for why this is polling-based rather than interrupt-driven), then
 * parses and forwards it. Runs entirely in this thread's own context --
 * there is no ISR handoff in this design, so no msgq is needed between
 * byte-collection and parsing; both happen right here. */
static void uart_cmd_task_entry(void *p1, void *p2, void *p3)
{
	char line_buf[IPC13_LINE_MAX];
	size_t line_len = 0;
	static uint32_t cmd_seq;
	uint8_t c;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	LOG_INF("[Uart_Cmd_Task] started -- type 1 (PING), 2 (STATUS), "
		"3 <0|1> (MODE), or 4 <0|1> (periodic LOG on/off) into this "
		"console and press Enter");

	while (1) {
		if (uart_poll_in(console_dev, &c) != 0) {
			/* No byte available right now -- back off briefly
			 * instead of busy-spinning on an idle UART. */
			k_msleep(UART_POLL_INTERVAL_MS);
			continue;
		}

		if (c == '\n' || c == '\r') {
			if (line_len == 0) {
				continue;
			}
			line_buf[line_len] = '\0';
			line_len = 0;
		} else {
			/* Silently drop bytes past IPC13_LINE_MAX-1 rather
			 * than blocking or growing the buffer -- an
			 * over-long line is truncated, not rejected; the
			 * operator will just see an "unknown command" reply
			 * for the truncated text. */
			if (line_len < IPC13_LINE_MAX - 1) {
				line_buf[line_len++] = (char)c;
			}
			continue;
		}

		if (handle_local_command(line_buf)) {
			continue;
		}

		struct ipc13_cmd_payload cmd;

		if (!parse_command(line_buf, &cmd)) {
			/* Rejected locally -- M55 never even hears about a
			 * command it wouldn't understand, so its command
			 * handling can stay a plain, always-valid switch. */
			printk("[M4] unknown command: \"%s\" (try: 1=PING, 2=STATUS, "
			       "3 <0|1>=MODE, 4 <0|1>=LOG on/off)\n",
			       line_buf);
			continue;
		}

		cmd.seq = ++cmd_seq;

		struct ipc13_msg msg = {.type = IPC13_MSG_CMD, .cmd = cmd};
		struct mbox_msg mbox_msg = {.data = &msg, .size = sizeof(msg)};

		k_mutex_lock(&tx_lock, K_FOREVER);
		mbox_send_dt(&tx_channel, &mbox_msg);
		k_mutex_unlock(&tx_lock);

		LOG_INF("[Uart_Cmd_Task] forwarded cmd=%u arg=%u seq=%u to M55",
			cmd.cmd, cmd.arg, cmd.seq);
	}
}

static void resp_print_task_entry(void *p1, void *p2, void *p3)
{
	struct ipc13_resp_payload resp;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	LOG_INF("[Resp_Print_Task] started");

	while (1) {
		if (k_msgq_get(&resp_msgq, &resp, K_FOREVER) != 0) {
			continue;
		}
		/* printk (not LOG_INF) so the operator's reply visually
		 * stands out from the timestamped LOG_* lines around it. */
		printk("[M55] %s%s\n", resp.text, resp.ok ? "" : " (rejected)");
	}
}

static void mbox_rx_callback(const struct device *dev, mbox_channel_id_t channel_id,
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

	if (msg.type == IPC13_MSG_RESP) {
		k_msgq_put(&resp_msgq, &msg.resp, K_NO_WAIT);
	}
	/* Any other type isn't expected from M55 in this lab -- ignored. */
}

/* ---------------------------------------------------------- main logic */

int main(void)
{
	bool have_aht20 = false, have_bmp280 = false;
	uint8_t sensor_mask = 0;
	uint32_t seq = 0;
	struct mbox_dt_spec rx_channel;

	LOG_INF("Lab13 UART Bridge CLIENT - %s", CONFIG_BOARD_TARGET);

	k_mutex_init(&tx_lock);

	tx_channel = (struct mbox_dt_spec)MBOX_DT_SPEC_GET(DT_PATH(mbox_consumer), tx);
	rx_channel = (struct mbox_dt_spec)MBOX_DT_SPEC_GET(DT_PATH(mbox_consumer), rx);

	if (mbox_register_callback_dt(&rx_channel, mbox_rx_callback, NULL)) {
		LOG_ERR("mbox_register_callback() failed");
	}
	if (mbox_set_enabled_dt(&rx_channel, 1)) {
		LOG_ERR("mbox_set_enabled() failed");
	}

	if (!device_is_ready(console_dev)) {
		LOG_ERR("console UART not ready -- command intake disabled");
	}
	/* No interrupt setup here -- Uart_Cmd_Task polls console_dev itself
	 * via uart_poll_in(). See the file header comment for why: this
	 * UART instance's driver returns -ENOTSUP from
	 * uart_irq_callback_set(), confirmed on real hardware. */

	k_thread_create(&uart_task_data, uart_task_stack, K_THREAD_STACK_SIZEOF(uart_task_stack),
			uart_cmd_task_entry, NULL, NULL, NULL, UART_TASK_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&uart_task_data, "Uart_Cmd_Task");

	k_thread_create(&resp_task_data, resp_task_stack, K_THREAD_STACK_SIZEOF(resp_task_stack),
			resp_print_task_entry, NULL, NULL, NULL, RESP_TASK_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&resp_task_data, "Resp_Print_Task");

	if (!device_is_ready(i2c_bus)) {
		LOG_ERR("i2c0 device not ready (check devicetree status/overlay)");
	} else {
		if (aht20_probe()) {
			if (aht20_init() == 0) {
				have_aht20 = true;
				sensor_mask |= IPC13_SENSOR_AHT20;
				LOG_INF("AHT20 found at 0x%02x", AHT20_ADDR);
			}
		} else {
			LOG_WRN("AHT20 not found at 0x%02x", AHT20_ADDR);
		}

		static const uint8_t bmp280_candidates[] = {0x76, 0x77};

		for (size_t i = 0; i < ARRAY_SIZE(bmp280_candidates); i++) {
			if (bmp280_probe(bmp280_candidates[i])) {
				bmp280_addr = bmp280_candidates[i];
				if (bmp280_read_calib() == 0 && bmp280_configure() == 0) {
					have_bmp280 = true;
					sensor_mask |= IPC13_SENSOR_BMP280;
					LOG_INF("BMP280/BME280 found at 0x%02x", bmp280_addr);
				}
				break;
			}
		}

		if (!have_aht20 && !have_bmp280) {
			LOG_WRN("no sensors found on i2c0 -- check wiring/address scan");
		}
	}

	int64_t sum_temp = 0, sum_hum = 0, sum_press = 0;
	int temp_count = 0, hum_count = 0, press_count = 0;
	int tick = 0;
	int32_t last_sent_temp = 0, last_sent_press = 0;
	bool have_last_sent = false;
	uint8_t last_sent_error_mask = 0;
	uint32_t aht20_fail_streak = 0, bmp280_fail_streak = 0;

	while (1) {
		int32_t aht_temp = 0, aht_hum = 0;
		int32_t bmp_temp = 0, bmp_press = 0;
		bool got_temp = false, got_hum = false, got_press = false;
		uint8_t error_mask = 0;

		if (have_aht20) {
			if (aht20_read(&aht_temp, &aht_hum) == 0) {
				got_temp = true;
				got_hum = true;
				aht20_fail_streak = 0;
			} else {
				error_mask |= IPC13_ERROR_AHT20;
				aht20_fail_streak++;
				LOG_WRN("AHT20 read failed this tick (I2C error), streak=%u",
					aht20_fail_streak);

				if (aht20_fail_streak >= FAIL_RECOVER_THRESHOLD) {
					LOG_WRN("AHT20 failing repeatedly -- attempting bus/sensor recovery");
					int rec = i2c_recover_bus(i2c_bus);

					if (rec) {
						LOG_WRN("i2c_recover_bus() not supported/failed, ret=%d "
							"(continuing with re-init only)", rec);
					}
					(void)aht20_init();
					aht20_fail_streak = 0;
				}
			}
		}
		if (have_bmp280) {
			if (bmp280_read(&bmp_temp, &bmp_press) == 0) {
				got_press = true;
				bmp280_fail_streak = 0;
				if (!got_temp) {
					aht_temp = bmp_temp;
					got_temp = true;
				}
			} else {
				error_mask |= IPC13_ERROR_BMP280;
				bmp280_fail_streak++;
				LOG_WRN("BMP280 read failed this tick (I2C error), streak=%u",
					bmp280_fail_streak);

				if (bmp280_fail_streak >= FAIL_RECOVER_THRESHOLD) {
					LOG_WRN("BMP280 failing repeatedly -- attempting bus/sensor recovery");
					int rec = i2c_recover_bus(i2c_bus);

					if (rec) {
						LOG_WRN("i2c_recover_bus() not supported/failed, ret=%d "
							"(continuing with re-config only)", rec);
					}
					(void)bmp280_configure();
					bmp280_fail_streak = 0;
				}
			}
		}

		if (got_temp) {
			sum_temp += aht_temp;
			temp_count++;
		}
		if (got_hum) {
			sum_hum += aht_hum;
			hum_count++;
		}
		if (got_press) {
			sum_press += bmp_press;
			press_count++;
		}

		if (error_mask != last_sent_error_mask) {
			send_env(got_temp ? aht_temp : last_sent_temp,
				 got_hum ? aht_hum : 0,
				 got_press ? bmp_press : last_sent_press,
				 sensor_mask, error_mask, IPC13_REASON_SENSOR_ERROR, ++seq);
			LOG_INF("sensor error state changed: 0x%x -> 0x%x, event sent",
				last_sent_error_mask, error_mask);
			last_sent_error_mask = error_mask;
		}

		if (have_last_sent && got_temp) {
			int32_t delta = aht_temp - last_sent_temp;

			if (abs(delta) >= TEMP_JUMP_MILLI_C) {
				send_env(aht_temp, got_hum ? aht_hum : 0, got_press ? bmp_press : 0,
					 sensor_mask, error_mask, IPC13_REASON_TEMP_JUMP, ++seq);
				last_sent_temp = aht_temp;
			}
		}
		if (have_last_sent && got_press) {
			int32_t delta = bmp_press - last_sent_press;

			if (abs(delta) >= PRESSURE_JUMP_MILLI_HPA) {
				send_env(got_temp ? aht_temp : last_sent_temp, got_hum ? aht_hum : 0,
					 bmp_press, sensor_mask, error_mask, IPC13_REASON_PRESSURE_JUMP, ++seq);
				last_sent_press = bmp_press;
			}
		}

		tick++;
		if (tick >= SAMPLES_PER_AVERAGE) {
			int32_t avg_temp = temp_count ? (int32_t)(sum_temp / temp_count) : last_sent_temp;
			int32_t avg_hum = hum_count ? (int32_t)(sum_hum / hum_count) : 0;
			int32_t avg_press = press_count ? (int32_t)(sum_press / press_count) : last_sent_press;

			send_env(avg_temp, avg_hum, avg_press, sensor_mask, error_mask,
				 IPC13_REASON_PERIODIC, ++seq);
			/* Gated by command "4 <0|1>" (see periodic_log_enabled) --
			 * telemetry to M55 above is unaffected either way, this
			 * only controls console noise on M4's own log. */
			if (atomic_get(&periodic_log_enabled)) {
				LOG_INF("periodic avg seq=%u temp=%d hum=%d press=%d mask=0x%x err=0x%x",
					seq, avg_temp, avg_hum, avg_press, sensor_mask, error_mask);
			}

			last_sent_temp = avg_temp;
			last_sent_press = avg_press;
			have_last_sent = true;

			sum_temp = sum_hum = sum_press = 0;
			temp_count = hum_count = press_count = 0;
			tick = 0;
		}

		k_msleep(SAMPLE_PERIOD_MS);
	}
	return 0;
}

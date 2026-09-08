/*
 * Lab 15: Low Power Sync (CLIENT, M4)
 *
 * This lab reproduces this SoC's actual intended power/role split: M4 is
 * the Always-On core, cheaply and continuously watching a lightweight
 * sensor, while M55 (which owns power-hungry blocks like the NPU) is
 * meant to stay idle and only be woken for something that actually
 * matters. Earlier labs (11-14) never modeled this -- Lab 11/12 have M4
 * push sensor data to M55 on every sample, and Lab 13's periodic telemetry
 * does the same. This lab is the first to send an mbox message ONLY on a
 * meaningful state change (a temperature threshold crossing), not on a
 * timer.
 *
 * SENSOR: reuses ONLY the AHT20 from Lab 12/13's combo module on &i2c0 --
 * BMP280 is intentionally left unused here. This lab's subject is the
 * always-on-monitor / wake-on-event IPC pattern itself, and one simple
 * scalar (temperature) is enough to demonstrate it; adding a second
 * sensor's threshold logic would only add noise. The AHT20 driver code
 * below (probe/init/read) is IDENTICAL to Lab 12/13's -- see those labs'
 * docs for the full I2C protocol explanation.
 *
 * COMMAND CHANNEL: reuses Lab 13/14's proven uart_poll_in() polling design
 * on the console UART. Exactly like Lab 14, ALL commands here are
 * M4-local -- setting/reading the threshold is purely M4's own business,
 * so nothing here crosses mbox either.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "ipc_common.h"

LOG_MODULE_REGISTER(lab15_client, CONFIG_LOG_DEFAULT_LEVEL);

/* How often M4 samples the AHT20. This is the "Always-On, cheap, frequent"
 * side of the pattern -- sampling itself never touches mbox, so a short
 * period here costs nothing on the M55/mbox side. */
#define SAMPLE_PERIOD_MS       500
#define FAIL_RECOVER_THRESHOLD 5

/* Default threshold, in whole degrees C. Changeable at runtime with
 * command "1 <celsius>". 28C is a reasonable indoor default that a
 * warm hand or a nearby heat source can cross for a live demo. */
#define DEFAULT_THRESHOLD_C 28

#define UART_TASK_STACK_SIZE 1536
#define UART_TASK_PRIORITY   5

static const struct device *i2c_bus = DEVICE_DT_GET(DT_NODELABEL(i2c0));
static struct mbox_dt_spec tx_channel;
static const struct device *console_dev = DEVICE_DT_GET(DT_NODELABEL(ns16550_uart0));

/* threshold_milli_c fits comfortably in a plain 32-bit value, so atomic_t
 * (contrast with Lab 14's pause_until_ms, a 64-bit uptime timestamp that
 * needed a k_mutex instead) is the right tool here -- same reasoning as
 * Lab 13's display_mode flag. */
static atomic_t threshold_milli_c = ATOMIC_INIT(DEFAULT_THRESHOLD_C * 1000);

/* Last known reading and state, for the M4-local "2" status command --
 * updated only by the sensor-sampling loop in main(), read only by
 * Uart_Cmd_Task. A single int32_t/enum pair like this is small enough
 * that a torn read is not a practical concern on this platform, but the
 * mutex keeps the pair internally consistent (temp and state always
 * describe the same sample). */
static struct k_mutex last_lock;
static int32_t last_temp_milli_c;
static enum ipc15_temp_state last_state = IPC15_TEMP_BELOW;
static bool have_last_reading;
static uint32_t event_count;

/* ---------------------------------------------------------------- AHT20 */
/* Identical to Lab 12/13's AHT20 driver -- see those labs' docs for the
 * full I2C register-level protocol explanation. */

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

/* --------------------------------------------------------- M4-local UI */

static void handle_command(const char *line)
{
	int celsius;

	if (sscanf(line, "1 %d", &celsius) == 1) {
		atomic_set(&threshold_milli_c, celsius * 1000);
		printk("[M4] threshold set to %dC (M4-local -- not sent to M55)\n", celsius);
		return;
	}

	if (strcmp(line, "2") == 0) {
		int32_t temp, thresh = atomic_get(&threshold_milli_c);
		enum ipc15_temp_state state;
		bool valid;

		k_mutex_lock(&last_lock, K_FOREVER);
		temp = last_temp_milli_c;
		state = last_state;
		valid = have_last_reading;
		k_mutex_unlock(&last_lock);

		if (!valid) {
			printk("[M4] status: no reading yet\n");
			return;
		}
		printk("[M4] status: temp=%d.%03dC threshold=%d.%03dC state=%s events_sent=%u\n",
		       temp / 1000, abs(temp % 1000), thresh / 1000, abs(thresh % 1000),
		       state == IPC15_TEMP_ABOVE ? "ABOVE" : "BELOW", event_count);
		return;
	}

	printk("[M4] unknown command: \"%s\" (try: 1 <celsius>=set threshold, 2=status)\n", line);
}

#define CMD_LINE_MAX 32
#define UART_POLL_INTERVAL_MS 10

K_THREAD_STACK_DEFINE(uart_task_stack, UART_TASK_STACK_SIZE);
static struct k_thread uart_task_data;

/* Same uart_poll_in() polling design as Lab 13/14 -- this UART instance's
 * driver returns -ENOTSUP for uart_irq_callback_set() on real hardware
 * (see Lab 13's troubleshooting doc), so interrupt-driven RX is not an
 * option here. */
static void uart_cmd_task_entry(void *p1, void *p2, void *p3)
{
	char line_buf[CMD_LINE_MAX];
	size_t line_len = 0;
	uint8_t c;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	LOG_INF("[Uart_Cmd_Task] started -- type 1 <celsius> (set threshold) or 2 (status) "
		"into this console and press Enter");

	while (1) {
		if (uart_poll_in(console_dev, &c) != 0) {
			k_msleep(UART_POLL_INTERVAL_MS);
			continue;
		}

		if (c == '\n' || c == '\r') {
			if (line_len == 0) {
				continue;
			}
			line_buf[line_len] = '\0';
			line_len = 0;
			handle_command(line_buf);
		} else if (line_len < CMD_LINE_MAX - 1) {
			line_buf[line_len++] = (char)c;
		}
	}
}

/* ---------------------------------------------------------- main logic */

int main(void)
{
	bool have_aht20 = false;
	uint32_t aht20_fail_streak = 0;
	bool have_state = false;
	enum ipc15_temp_state current_state = IPC15_TEMP_BELOW;

	LOG_INF("Lab15 Low Power Sync CLIENT - %s", CONFIG_BOARD_TARGET);

	k_mutex_init(&last_lock);

	tx_channel = (struct mbox_dt_spec)MBOX_DT_SPEC_GET(DT_PATH(mbox_consumer), tx);

	if (!device_is_ready(console_dev)) {
		LOG_ERR("console UART not ready -- command intake disabled");
	}

	k_thread_create(&uart_task_data, uart_task_stack, K_THREAD_STACK_SIZEOF(uart_task_stack),
			uart_cmd_task_entry, NULL, NULL, NULL, UART_TASK_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&uart_task_data, "Uart_Cmd_Task");

	if (!device_is_ready(i2c_bus)) {
		LOG_ERR("i2c0 device not ready (check devicetree status/overlay)");
	} else if (aht20_probe()) {
		if (aht20_init() == 0) {
			have_aht20 = true;
			LOG_INF("AHT20 found at 0x%02x", AHT20_ADDR);
		}
	} else {
		LOG_WRN("AHT20 not found at 0x%02x -- no threshold events will be sent", AHT20_ADDR);
	}

	while (1) {
		if (have_aht20) {
			int32_t temp, hum;

			if (aht20_read(&temp, &hum) == 0) {
				aht20_fail_streak = 0;

				int32_t thresh = atomic_get(&threshold_milli_c);
				enum ipc15_temp_state new_state =
					(temp >= thresh) ? IPC15_TEMP_ABOVE : IPC15_TEMP_BELOW;

				k_mutex_lock(&last_lock, K_FOREVER);
				last_temp_milli_c = temp;
				last_state = new_state;
				have_last_reading = true;
				k_mutex_unlock(&last_lock);

				/* THE key line of this lab: send to M55 only when
				 * the state actually changed (including the very
				 * first reading, so M55 leaves WAITING promptly)
				 * -- never on every sample. Compare with Lab 13's
				 * unconditional periodic send. */
				if (!have_state || new_state != current_state) {
					struct ipc15_event_payload evt = {
						.seq = ++event_count,
						.temp_milli_c = temp,
						.state = (uint8_t)new_state,
					};
					struct mbox_msg mbox_msg = {.data = &evt, .size = sizeof(evt)};

					mbox_send_dt(&tx_channel, &mbox_msg);
					LOG_INF("state change: %s -> %s @ %d.%03dC, event seq=%u sent to M55",
						have_state ? (current_state == IPC15_TEMP_ABOVE ? "ABOVE" : "BELOW") : "(none)",
						new_state == IPC15_TEMP_ABOVE ? "ABOVE" : "BELOW",
						temp / 1000, abs(temp % 1000), event_count);

					current_state = new_state;
					have_state = true;
				}
			} else {
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

		k_msleep(SAMPLE_PERIOD_MS);
	}
	return 0;
}

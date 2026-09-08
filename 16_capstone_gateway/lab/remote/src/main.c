/*
 * Lab 16: Capstone Gateway (CLIENT, M4)
 *
 * This is the final lab in the curriculum, and it adds NO new hardware at
 * all -- every sensor, panel, and command channel below is exactly the
 * hardware already wired for Lab 09/10 (accelerometer, I2C1), Lab 12/13/15
 * (AHT20+BMP280, I2C0), Lab 11 (ST7789V3 TFT, M55's SPI0), and Lab 13/14/15
 * (M4 console UART command intake). This lab's job is combining the
 * PATTERNS those labs established, not adding new wiring.
 *
 * Four threads run concurrently on M4, each reusing a proven pattern:
 *
 *   Env_Task      -- samples AHT20+BMP280 on &i2c0 every 200ms. Reuses
 *                    Lab 12's raw-I2C dual-sensor driver verbatim (AHT20
 *                    primary, BMP280 temperature as fallback, per-sensor
 *                    error tracking, active i2c_recover_bus()+re-init
 *                    after FAIL_RECOVER_THRESHOLD consecutive failures).
 *                    Unlike Lab 12/13 (which also sent a periodic average
 *                    every ~1s), this task follows Lab 15's discipline:
 *                    mbox_send_dt() only fires on an actual state change
 *                    (temperature crosses the threshold) or an error_mask
 *                    change -- never on a fixed timer.
 *
 *   Motion_Task   -- samples the MC3419 accelerometer on &i2c1 every 100ms.
 *                    Reuses Lab 10's boot-time baseline calibration (to
 *                    cancel the constant ~1g gravity offset) and debounced
 *                    threshold comparison, again with a Lab-15-style
 *                    state-change-only send.
 *
 *   Heartbeat_Task -- sends a plain seq-numbered beacon every 1000ms.
 *                     Exactly Lab 14's heartbeat pattern -- M55 is the one
 *                     that watches for its absence (see the HOST side).
 *
 *   Uart_Cmd_Task  -- polls the console UART for operator commands, exactly
 *                     Lab 13/14/15's uart_poll_in() polling design (this
 *                     UART instance's driver returns -ENOTSUP for
 *                     uart_irq_callback_set() on real hardware -- see Lab
 *                     13's troubleshooting doc). Commands 1/2/3 are
 *                     M4-local (matching Lab 14/15); command 4 is the one
 *                     command that IS relayed to M55 (an on-demand
 *                     IPC16_MSG_STATUS push) -- see ipc_common.h's file
 *                     header for why this is a deliberate, per-command
 *                     choice, not an inconsistency.
 *
 * All four threads can call mbox_send_dt() on the same tx_channel, so
 * tx_lock (a k_mutex, same as Lab 13) serializes them.
 *
 * Every mbox rx/tx call in this file follows the curriculum's one
 * absolute rule since Lab 07: this lab doesn't register an mbox rx
 * callback on M4 at all (M55 never sends anything down in this lab), so
 * there is no ISR-context code here to begin with -- all four threads
 * above run in ordinary thread context, and it stays that way.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "ipc_common.h"

LOG_MODULE_REGISTER(lab16_client, CONFIG_LOG_DEFAULT_LEVEL);

static const struct device *i2c0_bus = DEVICE_DT_GET(DT_NODELABEL(i2c0));
static const struct device *accel_dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(mc3479));
static const struct device *console_dev = DEVICE_DT_GET(DT_NODELABEL(ns16550_uart0));

static struct mbox_dt_spec tx_channel;
static struct k_mutex tx_lock;

static void send_msg(struct ipc16_msg *msg)
{
	struct mbox_msg mbox_msg = {.data = msg, .size = sizeof(*msg)};

	k_mutex_lock(&tx_lock, K_FOREVER);
	mbox_send_dt(&tx_channel, &mbox_msg);
	k_mutex_unlock(&tx_lock);
}

/* ============================================================= ENV_TASK */
/* AHT20 + BMP280 raw I2C driver -- IDENTICAL to Lab 12/13/15's. See those
 * labs' docs for the full register-level protocol explanation; reproduced
 * here only where behavior differs (state-change-only send, below). */

#define ENV_SAMPLE_PERIOD_MS    200
#define ENV_FAIL_RECOVER_THRESH 5
#define DEFAULT_ENV_THRESHOLD_C 28

#define AHT20_ADDR            0x38
#define AHT20_CMD_SOFT_RESET  0xBA
#define AHT20_CMD_INIT        0xBE
#define AHT20_CMD_TRIGGER     0xAC
#define AHT20_STATUS_BUSY_BIT 0x80

static bool aht20_probe(void)
{
	uint8_t dummy;

	return i2c_read(i2c0_bus, &dummy, 1, AHT20_ADDR) == 0;
}

static int aht20_init(void)
{
	uint8_t reset_cmd = AHT20_CMD_SOFT_RESET;
	uint8_t init_cmd[3] = {AHT20_CMD_INIT, 0x08, 0x00};
	int ret;

	ret = i2c_write(i2c0_bus, &reset_cmd, 1, AHT20_ADDR);
	if (ret) return ret;
	k_msleep(20);

	ret = i2c_write(i2c0_bus, init_cmd, sizeof(init_cmd), AHT20_ADDR);
	k_msleep(10);
	return ret;
}

static int aht20_read(int32_t *temp_milli_c, int32_t *humidity_milli_pct)
{
	uint8_t trigger_cmd[3] = {AHT20_CMD_TRIGGER, 0x33, 0x00};
	uint8_t data[6];
	int ret;

	ret = i2c_write(i2c0_bus, trigger_cmd, sizeof(trigger_cmd), AHT20_ADDR);
	if (ret) return ret;
	k_msleep(80);

	ret = i2c_read(i2c0_bus, data, sizeof(data), AHT20_ADDR);
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

#define BMP280_REG_CHIPID      0xD0
#define BMP280_REG_CTRL_MEAS   0xF4
#define BMP280_REG_CONFIG      0xF5
#define BMP280_REG_CALIB_START 0x88
#define BMP280_REG_PRESS_MSB   0xF7
#define BMP280_CHIPID_BMP280   0x58
#define BMP280_CHIPID_BME280   0x60

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

	if (i2c_write_read(i2c0_bus, addr, &reg, 1, &id, 1) != 0) {
		return false;
	}
	return id == BMP280_CHIPID_BMP280 || id == BMP280_CHIPID_BME280;
}

static int bmp280_read_calib(void)
{
	uint8_t reg = BMP280_REG_CALIB_START;
	uint8_t buf[24];
	int ret;

	ret = i2c_write_read(i2c0_bus, bmp280_addr, &reg, 1, buf, sizeof(buf));
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

	ret = i2c_write(i2c0_bus, ctrl_meas, sizeof(ctrl_meas), bmp280_addr);
	if (ret) return ret;
	return i2c_write(i2c0_bus, config, sizeof(config), bmp280_addr);
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

static int bmp280_read_temp_only(int32_t *temp_milli_c)
{
	uint8_t reg = BMP280_REG_PRESS_MSB;
	uint8_t buf[6];
	int32_t adc_T, t_fine;
	int ret;

	ret = i2c_write_read(i2c0_bus, bmp280_addr, &reg, 1, buf, sizeof(buf));
	if (ret) return ret;

	adc_T = ((int32_t)buf[3] << 12) | ((int32_t)buf[4] << 4) | (buf[5] >> 4);
	*temp_milli_c = bmp280_compensate_temp(adc_T, &t_fine) * 10;

	return 0;
}

/* Shared with Uart_Cmd_Task's "3"/"4" status commands, and with
 * Heartbeat_Task's beacon seq (read-only there). Protected by env_lock so
 * a torn read never mixes fields from two different samples. */
static struct k_mutex env_lock;
static int32_t g_env_temp_milli_c;
static int32_t g_env_humidity_milli_pct;
static uint8_t g_env_from_bmp;
static enum ipc16_env_state g_env_state = IPC16_ENV_BELOW;
static uint8_t g_env_error_mask;
static bool g_env_have_reading;
static atomic_t env_threshold_milli_c = ATOMIC_INIT(DEFAULT_ENV_THRESHOLD_C * 1000);

#define ENV_TASK_STACK_SIZE 2048
#define ENV_TASK_PRIORITY   5

K_THREAD_STACK_DEFINE(env_task_stack, ENV_TASK_STACK_SIZE);
static struct k_thread env_task_data;

static void env_task_entry(void *p1, void *p2, void *p3)
{
	bool have_aht20 = false, have_bmp280 = false;
	uint32_t aht20_fail_streak = 0, bmp280_fail_streak = 0;
	bool have_state = false;
	enum ipc16_env_state current_state = IPC16_ENV_BELOW;
	uint8_t last_error_mask = 0;
	uint32_t seq = 0;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	LOG_INF("[Env_Task] started");

	if (!device_is_ready(i2c0_bus)) {
		LOG_ERR("i2c0 device not ready (check devicetree status/overlay)");
	} else {
		if (aht20_probe()) {
			if (aht20_init() == 0) {
				have_aht20 = true;
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
					LOG_INF("BMP280/BME280 found at 0x%02x", bmp280_addr);
				}
				break;
			}
		}

		if (!have_aht20 && !have_bmp280) {
			LOG_WRN("no sensors found on i2c0 -- no ENV events will be sent");
		}
	}

	while (1) {
		int32_t aht_temp = 0, aht_hum = 0, bmp_temp = 0;
		bool got_temp = false, got_hum = false;
		uint8_t error_mask = 0;
		bool from_bmp = false;

		if (have_aht20) {
			if (aht20_read(&aht_temp, &aht_hum) == 0) {
				got_temp = true;
				got_hum = true;
				aht20_fail_streak = 0;
			} else {
				error_mask |= IPC16_ENV_ERROR_AHT20;
				aht20_fail_streak++;
				if (aht20_fail_streak >= ENV_FAIL_RECOVER_THRESH) {
					LOG_WRN("AHT20 failing repeatedly -- attempting bus/sensor recovery");
					(void)i2c_recover_bus(i2c0_bus);
					(void)aht20_init();
					aht20_fail_streak = 0;
				}
			}
		}
		if (have_bmp280) {
			if (bmp280_read_temp_only(&bmp_temp) == 0) {
				bmp280_fail_streak = 0;
				if (!got_temp) {
					/* AHT20 absent or its read failed this tick --
					 * BMP280's own temperature is a reasonable
					 * fallback (Lab 12's dual-sensor pattern). */
					aht_temp = bmp_temp;
					got_temp = true;
					from_bmp = true;
				}
			} else {
				error_mask |= IPC16_ENV_ERROR_BMP280;
				bmp280_fail_streak++;
				if (bmp280_fail_streak >= ENV_FAIL_RECOVER_THRESH) {
					LOG_WRN("BMP280 failing repeatedly -- attempting bus/sensor recovery");
					(void)i2c_recover_bus(i2c0_bus);
					(void)bmp280_configure();
					bmp280_fail_streak = 0;
				}
			}
		}

		if (got_temp) {
			int32_t thresh = atomic_get(&env_threshold_milli_c);
			enum ipc16_env_state new_state =
				(aht_temp >= thresh) ? IPC16_ENV_ABOVE : IPC16_ENV_BELOW;

			k_mutex_lock(&env_lock, K_FOREVER);
			g_env_temp_milli_c = aht_temp;
			g_env_humidity_milli_pct = got_hum ? aht_hum : 0;
			g_env_from_bmp = from_bmp;
			g_env_state = new_state;
			g_env_error_mask = error_mask;
			g_env_have_reading = true;
			k_mutex_unlock(&env_lock);

			/* State-change-only send (Lab 15 discipline), plus an
			 * immediate send whenever the error_mask itself changes
			 * (Lab 12's "two independent fault indicators" --
			 * M55 shouldn't have to guess whether silence means
			 * "stable reading" or "a sensor just died"). */
			if (!have_state || new_state != current_state ||
			    error_mask != last_error_mask) {
				struct ipc16_msg msg = {
					.type = IPC16_MSG_ENV,
					.payload.env = {
						.temp_milli_c = aht_temp,
						.humidity_milli_pct = got_hum ? aht_hum : 0,
						.from_bmp = from_bmp,
						.state = (uint8_t)new_state,
						.error_mask = error_mask,
						.seq = ++seq,
					},
				};

				send_msg(&msg);
				LOG_INF("[Env_Task] event seq=%u temp=%d%s state=%s err=0x%x",
					seq, aht_temp, from_bmp ? "(bmp)" : "",
					new_state == IPC16_ENV_ABOVE ? "ABOVE" : "BELOW",
					error_mask);

				current_state = new_state;
				have_state = true;
				last_error_mask = error_mask;
			}
		}

		k_msleep(ENV_SAMPLE_PERIOD_MS);
	}
}

/* ========================================================== MOTION_TASK */
/* Accelerometer threshold-crossing detector -- reuses Lab 10's baseline
 * calibration and debounce logic verbatim. */

#define MOTION_POLL_PERIOD_MS    100
#define MOTION_DEBOUNCE_MS       500
#define MOTION_CALIB_SAMPLES     10 /* ~1s of samples at POLL_PERIOD_MS each */
#define DEFAULT_MOTION_THRESH_MG 2000

static atomic_t motion_threshold_milli_g = ATOMIC_INIT(DEFAULT_MOTION_THRESH_MG);

static struct k_mutex motion_lock;
static int32_t g_motion_mag_milli_g;
static enum ipc16_motion_state g_motion_state = IPC16_MOTION_QUIET;
static bool g_motion_have_reading;

#define MOTION_TASK_STACK_SIZE 1536
#define MOTION_TASK_PRIORITY   5

K_THREAD_STACK_DEFINE(motion_task_stack, MOTION_TASK_STACK_SIZE);
static struct k_thread motion_task_data;

static int32_t sensor_val_to_milli(struct sensor_value *v)
{
	return v->val1 * 1000 + v->val2 / 1000;
}

static int read_accel_milli(int32_t *x, int32_t *y, int32_t *z)
{
	struct sensor_value ax, ay, az;

	if (accel_dev == NULL || !device_is_ready(accel_dev)) {
		return -ENODEV;
	}
	if (sensor_sample_fetch(accel_dev) != 0) {
		return -EIO;
	}
	sensor_channel_get(accel_dev, SENSOR_CHAN_ACCEL_X, &ax);
	sensor_channel_get(accel_dev, SENSOR_CHAN_ACCEL_Y, &ay);
	sensor_channel_get(accel_dev, SENSOR_CHAN_ACCEL_Z, &az);

	*x = sensor_val_to_milli(&ax);
	*y = sensor_val_to_milli(&ay);
	*z = sensor_val_to_milli(&az);
	return 0;
}

static void calibrate_baseline(int32_t *base_x, int32_t *base_y, int32_t *base_z)
{
	int64_t sum_x = 0, sum_y = 0, sum_z = 0;
	int good_samples = 0;
	int32_t x, y, z;

	LOG_INF("[Motion_Task] calibrating baseline (~1s, keep the board still)...");

	for (int i = 0; i < MOTION_CALIB_SAMPLES; i++) {
		if (read_accel_milli(&x, &y, &z) == 0) {
			sum_x += x;
			sum_y += y;
			sum_z += z;
			good_samples++;
		}
		k_msleep(MOTION_POLL_PERIOD_MS);
	}

	if (good_samples == 0) {
		*base_x = *base_y = *base_z = 0;
		LOG_WRN("[Motion_Task] calibration got no valid samples, baseline=0,0,0");
		return;
	}

	*base_x = (int32_t)(sum_x / good_samples);
	*base_y = (int32_t)(sum_y / good_samples);
	*base_z = (int32_t)(sum_z / good_samples);
	LOG_INF("[Motion_Task] baseline x=%d y=%d z=%d (from %d samples)",
		*base_x, *base_y, *base_z, good_samples);
}

static void motion_task_entry(void *p1, void *p2, void *p3)
{
	int32_t base_x, base_y, base_z;
	int64_t last_event_ms = 0;
	bool have_state = false;
	enum ipc16_motion_state current_state = IPC16_MOTION_QUIET;
	uint32_t seq = 0;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	LOG_INF("[Motion_Task] started");

	if (accel_dev == NULL || !device_is_ready(accel_dev)) {
		LOG_ERR("accel device not ready (check DT_NODELABEL(mc3479)) -- "
			"no MOTION events will be sent");
	} else {
		struct sensor_value odr = {.val1 = 50, .val2 = 0};
		struct sensor_value range = {.val1 = 0, .val2 = 0};

		/* Both calls required to get real (non-zero) data -- confirmed
		 * on real hardware in Lab 09/10. */
		if (sensor_attr_set(accel_dev, SENSOR_CHAN_ACCEL_XYZ,
				     SENSOR_ATTR_SAMPLING_FREQUENCY, &odr) != 0) {
			LOG_ERR("sensor_attr_set(SAMPLING_FREQUENCY) failed");
		}
		if (sensor_attr_set(accel_dev, SENSOR_CHAN_ACCEL_XYZ,
				     SENSOR_ATTR_FULL_SCALE, &range) != 0) {
			LOG_ERR("sensor_attr_set(FULL_SCALE) failed");
		}
	}

	calibrate_baseline(&base_x, &base_y, &base_z);

	while (1) {
		int32_t x, y, z;

		if (read_accel_milli(&x, &y, &z) == 0) {
			int32_t dx = x - base_x, dy = y - base_y, dz = z - base_z;
			int32_t mag = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy) + (dz < 0 ? -dz : dz);
			int32_t thresh = atomic_get(&motion_threshold_milli_g);
			enum ipc16_motion_state new_state =
				(mag > thresh) ? IPC16_MOTION_ACTIVE : IPC16_MOTION_QUIET;
			int64_t now = k_uptime_get();

			k_mutex_lock(&motion_lock, K_FOREVER);
			g_motion_mag_milli_g = mag;
			g_motion_state = new_state;
			g_motion_have_reading = true;
			k_mutex_unlock(&motion_lock);

			if ((!have_state || new_state != current_state) &&
			    (now - last_event_ms) > MOTION_DEBOUNCE_MS) {
				struct ipc16_msg msg = {
					.type = IPC16_MSG_MOTION,
					.payload.motion = {
						.mag_milli_g = mag,
						.state = (uint8_t)new_state,
						.seq = ++seq,
					},
				};

				send_msg(&msg);
				LOG_INF("[Motion_Task] event seq=%u mag=%d state=%s",
					seq, mag,
					new_state == IPC16_MOTION_ACTIVE ? "ACTIVE" : "QUIET");

				current_state = new_state;
				have_state = true;
				last_event_ms = now;
			}
		}

		k_msleep(MOTION_POLL_PERIOD_MS);
	}
}

/* ======================================================= HEARTBEAT_TASK */

#define HEARTBEAT_PERIOD_MS 1000

static atomic_t g_heartbeat_seq = ATOMIC_INIT(0);

#define HEARTBEAT_TASK_STACK_SIZE 512
#define HEARTBEAT_TASK_PRIORITY   5

K_THREAD_STACK_DEFINE(heartbeat_task_stack, HEARTBEAT_TASK_STACK_SIZE);
static struct k_thread heartbeat_task_data;

static void heartbeat_task_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	LOG_INF("[Heartbeat_Task] started");

	while (1) {
		uint32_t seq = (uint32_t)atomic_add(&g_heartbeat_seq, 1) + 1;
		struct ipc16_msg msg = {
			.type = IPC16_MSG_HEARTBEAT,
			.payload.heartbeat = {.seq = seq},
		};

		send_msg(&msg);
		k_msleep(HEARTBEAT_PERIOD_MS);
	}
}

/* ========================================================= UART_CMD_TASK */
/* Same uart_poll_in() polling design as Lab 13/14/15 -- this UART
 * instance's driver returns -ENOTSUP for uart_irq_callback_set() on real
 * hardware. */

#define UART_TASK_STACK_SIZE  1536
#define UART_TASK_PRIORITY    6
#define UART_POLL_INTERVAL_MS 10
#define CMD_LINE_MAX           32

K_THREAD_STACK_DEFINE(uart_task_stack, UART_TASK_STACK_SIZE);
static struct k_thread uart_task_data;

static void handle_command(const char *line)
{
	int arg;

	/* "1 <celsius>" -- M4-local: set the ENV threshold, same as Lab 15. */
	if (sscanf(line, "1 %d", &arg) == 1) {
		atomic_set(&env_threshold_milli_c, arg * 1000);
		printk("[M4] ENV threshold set to %dC (M4-local -- not sent to M55)\n", arg);
		return;
	}

	/* "2 <milli-g>" -- M4-local: set the MOTION threshold, same as Lab 10. */
	if (sscanf(line, "2 %d", &arg) == 1) {
		atomic_set(&motion_threshold_milli_g, arg);
		printk("[M4] MOTION threshold set to %d milli-g (M4-local -- not sent to M55)\n", arg);
		return;
	}

	/* "3" -- M4-local: print everything M4 currently knows to its own
	 * console. Nothing here touches mbox. */
	if (strcmp(line, "3") == 0) {
		int32_t temp, hum, mag;
		uint8_t from_bmp, err_mask;
		enum ipc16_env_state env_state;
		enum ipc16_motion_state motion_state;
		bool env_valid, motion_valid;

		k_mutex_lock(&env_lock, K_FOREVER);
		temp = g_env_temp_milli_c;
		hum = g_env_humidity_milli_pct;
		from_bmp = g_env_from_bmp;
		env_state = g_env_state;
		err_mask = g_env_error_mask;
		env_valid = g_env_have_reading;
		k_mutex_unlock(&env_lock);

		k_mutex_lock(&motion_lock, K_FOREVER);
		mag = g_motion_mag_milli_g;
		motion_state = g_motion_state;
		motion_valid = g_motion_have_reading;
		k_mutex_unlock(&motion_lock);

		printk("[M4] status: env=%s temp=%d.%03dC%s hum=%d.%03d%% err=0x%x | "
		       "motion=%s mag=%d milli-g | heartbeat_seq=%u | uptime=%llds\n",
		       env_valid ? (env_state == IPC16_ENV_ABOVE ? "ABOVE" : "BELOW") : "no-reading",
		       temp / 1000, abs(temp % 1000), from_bmp ? "(bmp)" : "",
		       hum / 1000, abs(hum % 1000), err_mask,
		       motion_valid ? (motion_state == IPC16_MOTION_ACTIVE ? "ACTIVE" : "QUIET") : "no-reading",
		       mag, (uint32_t)atomic_get(&g_heartbeat_seq),
		       k_uptime_get() / 1000);
		return;
	}

	/* "4" -- the ONE command in this lab that IS relayed to M55: pushes
	 * a full IPC16_MSG_STATUS snapshot on demand. See ipc_common.h. */
	if (strcmp(line, "4") == 0) {
		struct ipc16_status_payload status = {0};

		k_mutex_lock(&env_lock, K_FOREVER);
		status.temp_milli_c = g_env_temp_milli_c;
		status.humidity_milli_pct = g_env_humidity_milli_pct;
		status.from_bmp = g_env_from_bmp;
		status.env_state = (uint8_t)g_env_state;
		status.env_error_mask = g_env_error_mask;
		k_mutex_unlock(&env_lock);

		k_mutex_lock(&motion_lock, K_FOREVER);
		status.mag_milli_g = g_motion_mag_milli_g;
		status.motion_state = (uint8_t)g_motion_state;
		k_mutex_unlock(&motion_lock);

		status.heartbeat_seq = (uint32_t)atomic_get(&g_heartbeat_seq);
		status.uptime_sec = (uint32_t)(k_uptime_get() / 1000);

		struct ipc16_msg msg = {.type = IPC16_MSG_STATUS, .payload.status = status};

		send_msg(&msg);
		printk("[M4] STATUS pushed to M55 (relayed -- the one command in this lab that is)\n");
		return;
	}

	printk("[M4] unknown command: \"%s\" (try: 1 <celsius>=ENV threshold, "
	       "2 <milli-g>=MOTION threshold, 3=local status, 4=push STATUS to M55)\n",
	       line);
}

static void uart_cmd_task_entry(void *p1, void *p2, void *p3)
{
	char line_buf[CMD_LINE_MAX];
	size_t line_len = 0;
	uint8_t c;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	LOG_INF("[Uart_Cmd_Task] started -- type 1 <celsius>, 2 <milli-g>, 3, or 4 "
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

/* ================================================================= main */

int main(void)
{
	LOG_INF("Lab16 Capstone Gateway CLIENT - %s", CONFIG_BOARD_TARGET);

	k_mutex_init(&tx_lock);
	k_mutex_init(&env_lock);
	k_mutex_init(&motion_lock);

	tx_channel = (struct mbox_dt_spec)MBOX_DT_SPEC_GET(DT_PATH(mbox_consumer), tx);

	if (!device_is_ready(console_dev)) {
		LOG_ERR("console UART not ready -- command intake disabled");
	}

	k_thread_create(&env_task_data, env_task_stack, K_THREAD_STACK_SIZEOF(env_task_stack),
			env_task_entry, NULL, NULL, NULL, ENV_TASK_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&env_task_data, "Env_Task");

	k_thread_create(&motion_task_data, motion_task_stack,
			K_THREAD_STACK_SIZEOF(motion_task_stack),
			motion_task_entry, NULL, NULL, NULL, MOTION_TASK_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&motion_task_data, "Motion_Task");

	k_thread_create(&heartbeat_task_data, heartbeat_task_stack,
			K_THREAD_STACK_SIZEOF(heartbeat_task_stack),
			heartbeat_task_entry, NULL, NULL, NULL, HEARTBEAT_TASK_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&heartbeat_task_data, "Heartbeat_Task");

	k_thread_create(&uart_task_data, uart_task_stack, K_THREAD_STACK_SIZEOF(uart_task_stack),
			uart_cmd_task_entry, NULL, NULL, NULL, UART_TASK_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&uart_task_data, "Uart_Cmd_Task");

	while (1) {
		k_sleep(K_FOREVER);
	}
	return 0;
}

/*
 * Lab 12: AHT20 + BMP280 Environment Logging (CLIENT, M4)
 *
 * Reads an AHT20 (temperature/humidity) + BMP280 (pressure, also its own
 * temperature) combo module every 200ms, averages 5 samples (~1s) and
 * sends the average to M55, but also sends an immediate out-of-band
 * message the moment a single 200ms sample jumps too far from the last
 * value M55 was told about (temperature >= 0.5C, or pressure >= 50hPa).
 *
 * DESIGN CHOICE -- raw I2C, not the Zephyr sensor subsystem: both
 * sensors are talked to directly with i2c_write()/i2c_write_read() calls
 * instead of devicetree sensor nodes + sensor_sample_fetch()/
 * sensor_channel_get(). Two reasons:
 *   1. The user wants sensor I2C addresses auto-detected at boot rather
 *      than hardcoded (real-world modules ship with different addresses
 *      -- BMP280 in particular can be 0x76 or 0x77 depending on how its
 *      SDO pin is wired). The Zephyr sensor subsystem expects a fixed
 *      address per devicetree node, which doesn't fit that requirement.
 *   2. It sidesteps having to guess this Zephyr version's exact driver
 *      Kconfig symbol names (CONFIG_AHT20 / CONFIG_BMP280), which has
 *      been a recurring source of build-time surprises in earlier labs.
 *
 * HARDWARE ASSUMPTION (needs confirmation): the combo module is wired to
 * SoC GPIO3/GPIO4, assumed here to be &i2c0's pin route -- a SEPARATE
 * I2C controller from the &i2c1 bus every other lab in this curriculum
 * shares between M4/M55 (mc3479, gpio_exp0). If that assumption is
 * right, this lab doesn't need any of the usual "&i2c1 disabled on M55"
 * dance at all, since M55 never touches i2c0. See the M4 overlay for the
 * exact pinctrl TODO.
 *
 * AHT20 protocol (fixed I2C address 0x38, no address variants): soft
 * reset (0xBA) -> init (0xBE 0x08 0x00) -> [trigger (0xAC 0x33 0x00),
 * wait 80ms, read 6 bytes: status + 20-bit humidity + 20-bit temp] on
 * each measurement. This is the standard AHT20/AHT10 command set,
 * reproduced from the sensor's public datasheet.
 *
 * BMP280 protocol: chip ID register (0xD0, expect 0x58 for BMP280 or
 * 0x60 for the humidity-capable BME280 -- either is accepted here to
 * tolerate a BME280 being substituted) at whichever of 0x76/0x77 ACKs,
 * calibration data (0x88-0x9F, 24 bytes), ctrl_meas (0xF4) + config
 * (0xF5) to start normal-mode sampling, then raw pressure+temperature
 * from 0xF7-0xFC compensated with Bosch's standard 64-bit integer
 * formula (reproduced from the public BMP280 datasheet).
 *
 * NOTE: the compensation math and command sequences here follow the
 * public datasheets closely but have NOT yet been verified against this
 * specific module on real hardware -- if the pressure reading comes out
 * wildly outside ~950-1050 hPa, or temperature is off by more than a
 * degree or two from a reference thermometer, that's the first place to
 * check (see this lab's doc/troubleshooting notes once written).
 *
 * SENSOR READ-ERROR REPORTING (separate from the mbox connection-timeout
 * check M55 already does on its side): a sensor can be detected fine at
 * boot and still fail an individual I2C transaction later (a loose wire,
 * a bus glitch, a sensor that dropped off mid-measurement). Every 200ms
 * tick tracks, per sensor, whether THIS tick's read succeeded, and packs
 * that into `error_mask` on every outgoing message. The moment
 * error_mask changes (a sensor starts failing, or a failing sensor
 * recovers), an immediate IPC12_REASON_SENSOR_ERROR message is sent, the
 * same way a temperature/pressure jump gets its own immediate message --
 * M55 doesn't have to wait up to 1 second (or worse, 2 seconds and
 * mistake it for a full connection loss) to find out a specific sensor
 * is unhappy.
 *
 * ACTIVE RECOVERY, not just reporting: a plain retry-next-tick is NOT
 * enough for two real failure modes seen with I2C sensors that get
 * unplugged/replugged: (1) the I2C bus itself can get physically stuck
 * (a slave left holding SDA low mid-transaction) so every future
 * transaction keeps failing even after the wiring is fixed, until
 * something clocks the bus free again; (2) a sensor that briefly lost
 * power forgets its configuration (BMP280's ctrl_meas/config registers,
 * AHT20's init state) and needs to be set up again, not just re-read. So
 * after a sensor has failed FAIL_RECOVER_THRESHOLD ticks in a row, this
 * code calls `i2c_recover_bus()` (best-effort -- not all I2C controllers
 * implement it, a failure here is only logged) and re-runs that sensor's
 * init sequence, then resets the streak counter so a normal read is
 * tried again right away. This is what makes "unplug the sensor, plug it
 * back in" actually resume reporting real values instead of getting
 * stuck showing an error forever.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <stdlib.h>
#include <errno.h>
#include "ipc_common.h"

LOG_MODULE_REGISTER(lab12_client, CONFIG_LOG_DEFAULT_LEVEL);

#define SAMPLE_PERIOD_MS         200
#define SAMPLES_PER_AVERAGE      (1000 / SAMPLE_PERIOD_MS) /* = 5 */
#define TEMP_JUMP_MILLI_C        500     /* 0.5 C */
#define PRESSURE_JUMP_MILLI_HPA  50000   /* 50 hPa */

/* After this many CONSECUTIVE failed ticks (~1s at 200ms/tick), attempt an
 * active bus/sensor recovery instead of just continuing to retry plain
 * reads -- see the file header comment for why a plain retry alone isn't
 * enough (stuck I2C bus, or a sensor that forgot its config after a power
 * blip). */
#define FAIL_RECOVER_THRESHOLD 5

static const struct device *i2c_bus = DEVICE_DT_GET(DT_NODELABEL(i2c0));
static struct mbox_dt_spec tx_channel;

/* ---------------------------------------------------------------- AHT20 */

#define AHT20_ADDR            0x38
#define AHT20_CMD_SOFT_RESET  0xBA
#define AHT20_CMD_INIT        0xBE
#define AHT20_CMD_TRIGGER     0xAC
#define AHT20_STATUS_BUSY_BIT 0x80

static bool aht20_probe(void)
{
	uint8_t dummy;

	/* AHT20 has no chip-ID register -- a bare read is the only way to
	 * check for an ACK at this address before we've initialized it. */
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

	/* humidity(%) = raw / 2^20 * 100 -- scaled directly to milli-% to
	 * avoid needing floating point. */
	*humidity_milli_pct = (int32_t)(((uint64_t)raw_hum * 100000ULL) >> 20);
	/* temp(C) = raw / 2^20 * 200 - 50 -- scaled to milli-C. */
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
#define BMP280_CHIPID_BME280     0x60 /* accepted too -- register-compatible for our purposes */

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
	/* ctrl_meas: osrs_t=1, osrs_p=1, mode=normal (0b011) -> 0b001_001_11 */
	uint8_t ctrl_meas[2] = {BMP280_REG_CTRL_MEAS, 0x27};
	/* config: standby/filter left at power-on defaults (0) -- fine since
	 * we poll on our own 200ms schedule rather than relying on the
	 * sensor's internal standby timing. */
	uint8_t config[2] = {BMP280_REG_CONFIG, 0x00};
	int ret;

	ret = i2c_write(i2c_bus, ctrl_meas, sizeof(ctrl_meas), bmp280_addr);
	if (ret) return ret;
	return i2c_write(i2c_bus, config, sizeof(config), bmp280_addr);
}

/* Bosch's standard 64-bit integer compensation formula (BMP280
 * datasheet section 3.11.3), reproduced as-is. t_fine is an
 * intermediate value pressure compensation also needs. */
static int32_t bmp280_compensate_temp(int32_t adc_T, int32_t *t_fine)
{
	int32_t var1, var2;

	var1 = ((((adc_T >> 3) - ((int32_t)bmp280_calib.dig_T1 << 1))) *
		((int32_t)bmp280_calib.dig_T2)) >> 11;
	var2 = (((((adc_T >> 4) - ((int32_t)bmp280_calib.dig_T1)) *
		  ((adc_T >> 4) - ((int32_t)bmp280_calib.dig_T1))) >> 12) *
		((int32_t)bmp280_calib.dig_T3)) >> 14;
	*t_fine = var1 + var2;
	return (*t_fine * 5 + 128) >> 8; /* T in 0.01 C */
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
		return 0; /* avoid divide-by-zero */
	}
	p = 1048576 - adc_P;
	p = (((p << 31) - var2) * 3125) / var1;
	var1 = (((int64_t)bmp280_calib.dig_P9) * (p >> 13) * (p >> 13)) >> 25;
	var2 = (((int64_t)bmp280_calib.dig_P8) * p) >> 19;
	p = ((p + var1 + var2) >> 8) + (((int64_t)bmp280_calib.dig_P7) << 4);
	return (uint32_t)p; /* Pa in Q24.8 fixed point (value/256 = Pa) */
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
	/* pressure_q24_8 is Pa*256; milli-hPa = Pa*10 = pressure_q24_8*10/256 */
	*pressure_milli_hpa = (int32_t)(((int64_t)pressure_q24_8 * 10) / 256);

	return 0;
}

/* ---------------------------------------------------------- main logic */

static void send_msg(int32_t temp, int32_t hum, int32_t press, uint8_t mask,
		      uint8_t error_mask, enum ipc12_reason reason, uint32_t seq)
{
	struct ipc12_env_msg msg = {
		.temp_milli_c = temp,
		.humidity_milli_pct = hum,
		.pressure_milli_hpa = press,
		.seq = seq,
		.sensor_mask = mask,
		.error_mask = error_mask,
		.reason = reason,
	};
	struct mbox_msg mbox_msg = {.data = &msg, .size = sizeof(msg)};

	mbox_send_dt(&tx_channel, &mbox_msg);
}

int main(void)
{
	bool have_aht20 = false, have_bmp280 = false;
	uint8_t sensor_mask = 0;
	uint32_t seq = 0;

	LOG_INF("Lab12 Env Logging CLIENT - %s", CONFIG_BOARD_TARGET);

	tx_channel = (struct mbox_dt_spec)MBOX_DT_SPEC_GET(DT_PATH(mbox_consumer), tx);

	if (!device_is_ready(i2c_bus)) {
		LOG_ERR("i2c0 device not ready (check devicetree status/overlay)");
	} else {
		/* --- Boot-time address scan (per user's request: don't assume
		 * a fixed address, since real modules vary) --- */
		if (aht20_probe()) {
			if (aht20_init() == 0) {
				have_aht20 = true;
				sensor_mask |= IPC12_SENSOR_AHT20;
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
					sensor_mask |= IPC12_SENSOR_BMP280;
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
				error_mask |= IPC12_ERROR_AHT20;
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
					/* Re-run AHT20's init sequence in case it lost its
					 * configuration (e.g. a brief power drop) rather than
					 * just the bus being glitched. */
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
					/* AHT20 absent, or its read failed this tick --
					 * BMP280's own temperature is a reasonable fallback. */
					aht_temp = bmp_temp;
					got_temp = true;
				}
			} else {
				error_mask |= IPC12_ERROR_BMP280;
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
					/* Re-apply ctrl_meas/config in case BMP280 lost its
					 * configuration (e.g. a brief power drop). Calibration
					 * data (dig_T1..dig_T3, dig_P1..dig_P9) is read-only on
					 * the sensor and doesn't need to be re-read. */
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

		/* Immediate out-of-band push the moment a sensor that was found
		 * at boot starts failing to read, or a failing one recovers --
		 * same "don't make M55 wait for the next periodic average"
		 * rationale as the temp/pressure jump alerts below. Compared
		 * against the error_mask actually last SENT (not every tick's
		 * raw result), so a single transient blip and its immediate
		 * recovery on the very next tick still gets reported both ways,
		 * but we never send a duplicate message for an unchanged state. */
		if (error_mask != last_sent_error_mask) {
			send_msg(got_temp ? aht_temp : last_sent_temp,
				 got_hum ? aht_hum : 0,
				 got_press ? bmp_press : last_sent_press,
				 sensor_mask, error_mask, IPC12_REASON_SENSOR_ERROR, ++seq);
			LOG_INF("sensor error state changed: 0x%x -> 0x%x, event sent",
				last_sent_error_mask, error_mask);
			last_sent_error_mask = error_mask;
		}

		/* Immediate out-of-band push on a big single-sample jump,
		 * compared against the last value M55 was actually told about
		 * (not against the previous raw sample -- a slow drift across
		 * many small steps should NOT spam individual jump messages). */
		if (have_last_sent && got_temp) {
			int32_t delta = aht_temp - last_sent_temp;

			if (abs(delta) >= TEMP_JUMP_MILLI_C) {
				send_msg(aht_temp, got_hum ? aht_hum : 0, got_press ? bmp_press : 0,
					 sensor_mask, error_mask, IPC12_REASON_TEMP_JUMP, ++seq);
				last_sent_temp = aht_temp;
				LOG_INF("temp jump: %d -> %d milli-C, event sent", last_sent_temp - delta, aht_temp);
			}
		}
		if (have_last_sent && got_press) {
			int32_t delta = bmp_press - last_sent_press;

			if (abs(delta) >= PRESSURE_JUMP_MILLI_HPA) {
				send_msg(got_temp ? aht_temp : last_sent_temp, got_hum ? aht_hum : 0,
					 bmp_press, sensor_mask, error_mask, IPC12_REASON_PRESSURE_JUMP, ++seq);
				last_sent_press = bmp_press;
				LOG_INF("pressure jump: %d -> %d milli-hPa, event sent", last_sent_press - delta, bmp_press);
			}
		}

		tick++;
		if (tick >= SAMPLES_PER_AVERAGE) {
			int32_t avg_temp = temp_count ? (int32_t)(sum_temp / temp_count) : last_sent_temp;
			int32_t avg_hum = hum_count ? (int32_t)(sum_hum / hum_count) : 0;
			int32_t avg_press = press_count ? (int32_t)(sum_press / press_count) : last_sent_press;

			/* error_mask here reflects only this last tick, not the
			 * whole averaging window -- good enough to keep M55's
			 * sensor-health display current on every message, since
			 * any actual state CHANGE was already reported immediately
			 * above the moment it happened. */
			send_msg(avg_temp, avg_hum, avg_press, sensor_mask, error_mask, IPC12_REASON_PERIODIC, ++seq);
			LOG_INF("periodic avg seq=%u temp=%d hum=%d press=%d mask=0x%x err=0x%x",
				seq, avg_temp, avg_hum, avg_press, sensor_mask, error_mask);

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

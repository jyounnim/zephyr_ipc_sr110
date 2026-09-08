/*
 * Lab 11: TFT Sensor Display (CLIENT, M4)
 * Reads the onboard MC3419 accelerometer every 500ms and sends the values
 * to M55, which renders them on an ST7789V3 SPI TFT panel.
 *
 * This side is a direct reuse of Lab 09's accelerometer pipeline (same
 * node, same wake-up sequence, same milli-g conversion) -- what changed in
 * this lab is entirely on the M55 side: M55 now owns SPI0 to drive a TFT
 * panel instead of just logging the values to its console. See the M55
 * overlay/main.c and doc/ for why that also means M55 can no longer print
 * to its own serial console at the same time.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include "ipc_common.h"

LOG_MODULE_REGISTER(lab11_client, CONFIG_LOG_DEFAULT_LEVEL);

#define SAMPLE_PERIOD_MS 500

static struct mbox_dt_spec tx_channel;
static const struct device *accel_dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(mc3479));

static int32_t sensor_val_to_milli(struct sensor_value *v)
{
	return v->val1 * 1000 + v->val2 / 1000;
}

int main(void)
{
	struct ipc11_accel_msg msg = {0};
	struct sensor_value ax, ay, az;

	LOG_INF("Lab11 TFT Sensor Display CLIENT - %s", CONFIG_BOARD_TARGET);

	tx_channel = (struct mbox_dt_spec)MBOX_DT_SPEC_GET(DT_PATH(mbox_consumer), tx);

	if (accel_dev == NULL || !device_is_ready(accel_dev)) {
		LOG_ERR("accel device not ready (check DT_NODELABEL(mc3479))");
		return -ENODEV;
	}

	/* Same two-step wake sequence Lab 09 found necessary on real hardware:
	 * mc3419_init() leaves the sensor in low-power standby, and the
	 * driver's conversion "sensitivity" stays zero until the full-scale
	 * range is explicitly set. See Lab 09's troubleshooting doc for the
	 * full story. */
	struct sensor_value odr = {.val1 = 50, .val2 = 0}; /* 50 Hz */

	if (sensor_attr_set(accel_dev, SENSOR_CHAN_ACCEL_XYZ,
			     SENSOR_ATTR_SAMPLING_FREQUENCY, &odr) != 0) {
		LOG_ERR("sensor_attr_set(SAMPLING_FREQUENCY) failed -- accel may stay in standby");
	}

	struct sensor_value range = {.val1 = 0, .val2 = 0};

	if (sensor_attr_set(accel_dev, SENSOR_CHAN_ACCEL_XYZ,
			     SENSOR_ATTR_FULL_SCALE, &range) != 0) {
		LOG_ERR("sensor_attr_set(FULL_SCALE) failed -- readings may stay at 0");
	}

	while (1) {
		if (sensor_sample_fetch(accel_dev) == 0) {
			sensor_channel_get(accel_dev, SENSOR_CHAN_ACCEL_X, &ax);
			sensor_channel_get(accel_dev, SENSOR_CHAN_ACCEL_Y, &ay);
			sensor_channel_get(accel_dev, SENSOR_CHAN_ACCEL_Z, &az);

			msg.x = sensor_val_to_milli(&ax);
			msg.y = sensor_val_to_milli(&ay);
			msg.z = sensor_val_to_milli(&az);
			msg.seq++;

			struct mbox_msg mbox_msg = {.data = &msg, .size = sizeof(msg)};
			mbox_send_dt(&tx_channel, &mbox_msg);
		} else {
			LOG_WRN("sensor_sample_fetch failed");
		}

		k_msleep(SAMPLE_PERIOD_MS);
	}
	return 0;
}

#ifndef IPC_COMMON_H_
#define IPC_COMMON_H_
#include <zephyr/kernel.h>

/* sensor_mask: which sensors were detected (probed successfully) at boot.
 * This never changes after main() finishes its startup scan -- it does
 * NOT mean "still working right now", only "was found on the bus". */
#define IPC12_SENSOR_AHT20   BIT(0)
#define IPC12_SENSOR_BMP280  BIT(1)

/* error_mask: which sensors FAILED TO READ on the sample that produced
 * this specific message (or, for a periodic-average message, on the most
 * recent 200ms tick of the averaging window). This is the field that
 * distinguishes "sensor never found at boot" (sensor_mask bit clear,
 * error_mask meaningless) from "sensor was found at boot but is
 * currently failing to answer I2C" (sensor_mask bit set, error_mask bit
 * set) -- the latter is a NEW runtime fault M55 should show as an error,
 * separate from the mbox connection-timeout check. */
#define IPC12_ERROR_AHT20    BIT(0)
#define IPC12_ERROR_BMP280   BIT(1)

enum ipc12_reason {
	IPC12_REASON_PERIODIC = 0,       /* normal 1-second rolling average */
	IPC12_REASON_TEMP_JUMP = 1,      /* single-sample temp jump >= 0.5C */
	IPC12_REASON_PRESSURE_JUMP = 2,  /* single-sample pressure jump >= 50hPa */
	IPC12_REASON_SENSOR_ERROR = 3,   /* error_mask changed (onset or recovery) */
};

struct ipc12_env_msg {
	int32_t temp_milli_c;
	int32_t humidity_milli_pct;
	int32_t pressure_milli_hpa;
	uint32_t seq;
	uint8_t sensor_mask;
	uint8_t error_mask;
	uint8_t reason;
};
#endif

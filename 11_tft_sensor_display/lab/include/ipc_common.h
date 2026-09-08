#ifndef IPC_COMMON_H_
#define IPC_COMMON_H_

#include <zephyr/kernel.h>

/* M4 -> M55: accelerometer (X/Y/Z, milli-g) sampled from the onboard MC3419.
 * Reuses the exact same reading pipeline verified in Lab 09 -- this lab
 * changes what M55 *does* with the values (renders them on a TFT instead
 * of just logging them), not how M4 samples them. */
struct ipc11_accel_msg {
	int32_t x;
	int32_t y;
	int32_t z;
	uint32_t seq;
};

#endif /* IPC_COMMON_H_ */

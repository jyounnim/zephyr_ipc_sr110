#include <zephyr/sys/util.h>

/*
 * Lab 16: Capstone Gateway -- shared M4<->M55 message protocol.
 *
 * A single tagged union multiplexes every message type this lab's M4 sends
 * to M55 over one mbox tx/rx channel pair (same envelope-with-type-tag
 * pattern as Lab 06/13, needed here because -- unlike Lab 14/15, which each
 * carried exactly one message kind and so needed no tag at all -- this lab
 * genuinely multiplexes four different message kinds on one channel):
 *
 *   IPC16_MSG_ENV       -- AHT20+BMP280 threshold-crossing event. Reuses
 *                          Lab 12's raw-I2C dual-sensor driver (AHT20
 *                          primary, BMP280 temperature as fallback, a
 *                          per-sensor error_mask, and active bus/sensor
 *                          recovery after repeated failures), but -- like
 *                          Lab 15, not Lab 12/13 -- sends only on an actual
 *                          state change (threshold crossing) or an
 *                          error_mask change, never on a fixed period.
 *   IPC16_MSG_MOTION    -- accelerometer threshold-crossing event. Reuses
 *                          Lab 10's baseline-calibrated deviation check and
 *                          debounce, sent only on a state change (the same
 *                          state-change-only discipline as IPC16_MSG_ENV).
 *   IPC16_MSG_HEARTBEAT -- periodic liveness beacon, exactly Lab 14's
 *                          pattern (M55 watches for its ABSENCE with a
 *                          finite k_msgq_get() timeout).
 *   IPC16_MSG_STATUS    -- an on-demand full snapshot. Unlike the other
 *                          three (which M4 decides on its own to send),
 *                          this one only goes out when the operator types
 *                          command "4" into M4's console -- see this lab's
 *                          doc for why "should this command reach the
 *                          other core" is a per-command decision (a Lab 13
 *                          principle), not an all-or-nothing rule for the
 *                          whole lab.
 */

#ifndef IPC_COMMON_H_
#define IPC_COMMON_H_

#include <stdint.h>

enum ipc16_msg_type {
	IPC16_MSG_ENV = 0,
	IPC16_MSG_MOTION,
	IPC16_MSG_HEARTBEAT,
	IPC16_MSG_STATUS,
};

enum ipc16_env_state {
	IPC16_ENV_BELOW = 0,
	IPC16_ENV_ABOVE,
};

enum ipc16_motion_state {
	IPC16_MOTION_QUIET = 0,
	IPC16_MOTION_ACTIVE,
};

/* error_mask bits -- which sensor(s) failed THIS tick. Independent of
 * env_state (a threshold reading can be perfectly valid while the OTHER
 * sensor is down) -- see this lab's doc for the "two independent fault
 * indicators" principle carried over from Lab 12. */
#define IPC16_ENV_ERROR_AHT20  BIT(0)
#define IPC16_ENV_ERROR_BMP280 BIT(1)

struct ipc16_env_payload {
	int32_t temp_milli_c;
	int32_t humidity_milli_pct; /* 0 if this reading has no humidity (BMP280-only fallback) */
	uint8_t from_bmp;           /* 1 if temp_milli_c came from BMP280, not AHT20 */
	uint8_t state;               /* enum ipc16_env_state */
	uint8_t error_mask;          /* IPC16_ENV_ERROR_* bitmask, this tick only */
	uint32_t seq;
};

struct ipc16_motion_payload {
	int32_t mag_milli_g; /* |dev_x|+|dev_y|+|dev_z| from the boot-time baseline */
	uint8_t state;         /* enum ipc16_motion_state */
	uint32_t seq;
};

struct ipc16_heartbeat_payload {
	uint32_t seq;
};

struct ipc16_status_payload {
	int32_t temp_milli_c;
	int32_t humidity_milli_pct;
	uint8_t from_bmp;
	uint8_t env_state;
	uint8_t env_error_mask;
	int32_t mag_milli_g;
	uint8_t motion_state;
	uint32_t heartbeat_seq;
	uint32_t uptime_sec;
};

struct ipc16_msg {
	uint8_t type; /* enum ipc16_msg_type */
	union {
		struct ipc16_env_payload env;
		struct ipc16_motion_payload motion;
		struct ipc16_heartbeat_payload heartbeat;
		struct ipc16_status_payload status;
	} payload;
};

#endif /* IPC_COMMON_H_ */

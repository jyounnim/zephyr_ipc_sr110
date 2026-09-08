/*
 * Lab 15: Low Power Sync -- shared M4/M55 message definitions.
 *
 * Exactly ONE message kind flows on this lab's mbox channel, always M4 ->
 * M55: a threshold-crossing event. Like Lab 14, this lab deliberately does
 * NOT use a tagged union -- see this lab's doc for why.
 *
 * The key difference from Lab 14's heartbeat: M4 does NOT send this
 * message on every sensor sample. It sends it ONLY when the temperature
 * state actually changes (BELOW -> ABOVE or ABOVE -> BELOW), which is the
 * whole point of this lab -- M4 stays "always-on" and samples the sensor
 * continuously and cheaply, but M55 is only ever bothered (woken over
 * mbox) when something has actually changed. See the M4-local "2" status
 * command in remote/src/main.c for how an operator can still see the
 * live/unsent reading without generating mbox traffic.
 */

#ifndef IPC_COMMON_H_
#define IPC_COMMON_H_

#include <zephyr/types.h>

enum ipc15_temp_state {
	IPC15_TEMP_BELOW = 0,
	IPC15_TEMP_ABOVE = 1,
};

/* M4 -> M55, sent ONLY when `state` actually changes (see main.c on both
 * sides). `seq` counts these state-change events (not sensor samples), so
 * a jump in `seq` on M55's screen means "one more transition happened",
 * not "one more sample was taken". */
struct ipc15_event_payload {
	uint32_t seq;
	int32_t temp_milli_c;
	uint8_t state; /* enum ipc15_temp_state */
};

#endif /* IPC_COMMON_H_ */

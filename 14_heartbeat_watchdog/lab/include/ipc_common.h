/*
 * Lab 14: Heartbeat Watchdog -- shared M4/M55 message definitions.
 *
 * Only ONE message kind flows on this lab's mbox channel, always in the
 * same direction (M4 -> M55): a periodic heartbeat beacon. Unlike Lab 06
 * and Lab 13, which multiplex several message kinds on one channel with
 * a `type` tag, this lab deliberately does NOT use a tagged union -- a
 * tag only earns its keep when more than one message shape actually
 * shares a channel. Here there is exactly one, so the raw payload struct
 * is sent directly. See this lab's doc for that design note.
 */

#ifndef IPC_COMMON_H_
#define IPC_COMMON_H_

#include <zephyr/types.h>

/* M4 -> M55, every HEARTBEAT_PERIOD_MS (see M4's main.c). `seq` lets M55
 * (and the operator, via M4's console) tell a genuinely fresh heartbeat
 * apart from a stale/duplicated one, though this lab's mbox transport
 * does not actually duplicate or reorder messages -- `seq` is kept
 * mainly because a real-world watchdog beacon almost always carries one. */
struct ipc14_heartbeat_payload {
	uint32_t seq;
};

#endif /* IPC_COMMON_H_ */

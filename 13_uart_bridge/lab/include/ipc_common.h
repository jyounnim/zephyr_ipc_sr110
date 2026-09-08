#ifndef IPC_COMMON_H_
#define IPC_COMMON_H_

#include <zephyr/kernel.h>

/*
 * This lab multiplexes THREE kinds of traffic onto the same mbox
 * tx/rx channel pair (see the M4/M55 main.c files for how each side
 * uses its tx and rx channel):
 *
 *   - IPC13_MSG_ENV  (M4 -> M55): periodic/event sensor telemetry --
 *     structurally identical to Lab 12's ipc12_env_msg, just wrapped in
 *     the tagged envelope below so it can share the channel with the
 *     new command traffic.
 *   - IPC13_MSG_CMD  (M4 -> M55): one operator command, typed into a
 *     terminal connected to M4's own console UART and relayed here by
 *     M4's Uart_Cmd_Task.
 *   - IPC13_MSG_RESP (M55 -> M4): M55's answer to one IPC13_MSG_CMD.
 *
 * The `type` tag at the front of the envelope tells the receiver which
 * union member to read. This is the same "command type field" idea
 * Lab 06 introduced, used here to combine two genuinely different
 * traffic patterns -- one-way periodic telemetry, and two-way
 * request/response -- on a single channel instead of only multiplexing
 * variants of one request type.
 */
enum ipc13_msg_type {
	IPC13_MSG_ENV  = 0,
	IPC13_MSG_CMD  = 1,
	IPC13_MSG_RESP = 2,
};

/* --- Sensor bitmasks / reasons, unchanged from Lab 12 --- */
#define IPC13_SENSOR_AHT20   BIT(0)
#define IPC13_SENSOR_BMP280  BIT(1)
#define IPC13_ERROR_AHT20    BIT(0)
#define IPC13_ERROR_BMP280   BIT(1)

enum ipc13_env_reason {
	IPC13_REASON_PERIODIC      = 0,
	IPC13_REASON_TEMP_JUMP     = 1,
	IPC13_REASON_PRESSURE_JUMP = 2,
	IPC13_REASON_SENSOR_ERROR  = 3,
};

struct ipc13_env_payload {
	int32_t temp_milli_c;
	int32_t humidity_milli_pct;
	int32_t pressure_milli_hpa;
	uint32_t seq;
	uint8_t sensor_mask;
	uint8_t error_mask;
	uint8_t reason;
};

/* --- Operator command relay (new in this lab) --- */
/* Operator types a single-digit number (not a word) into M4's console --
 * 1=PING, 2=STATUS, 3 <0|1>=MODE. See parse_command() in M4's main.c.
 *
 * NOTE: command "4 <0|1>" (periodic console-log on/off) also exists but
 * is intentionally NOT in this enum and never crosses mbox at all --
 * M4's handle_local_command() in main.c handles it entirely on M4,
 * since it has nothing to do with M55. Not every operator command needs
 * (or should get) a round trip; see that function's comment. */
enum ipc13_cmd {
	IPC13_CMD_PING   = 0, /* no argument -- M55 answers "PONG" */
	IPC13_CMD_STATUS = 1, /* no argument -- M55 answers with the latest
			       * sensor reading + connection state as text */
	IPC13_CMD_MODE   = 2, /* arg = 0 or 1 -- selects what Lab 13's extra
			       * status line on the TFT shows (see M55's
			       * main.c draw_extra_line()) */
};

struct ipc13_cmd_payload {
	uint8_t cmd;  /* enum ipc13_cmd */
	uint8_t arg;  /* only meaningful for IPC13_CMD_MODE */
	uint32_t seq; /* echoed back in the matching RESP -- not used for
		       * reordering in this lab, but shows the pattern that a
		       * busier request/response protocol would need. */
};

#define IPC13_RESP_TEXT_MAX 40

struct ipc13_resp_payload {
	uint8_t cmd;  /* which IPC13_CMD_* this answers */
	uint8_t ok;   /* 1 = command accepted, 0 = rejected (see text) */
	uint32_t seq; /* copied from the matching ipc13_cmd_payload.seq */
	char text[IPC13_RESP_TEXT_MAX];
};

struct ipc13_msg {
	uint8_t type; /* enum ipc13_msg_type */
	union {
		struct ipc13_env_payload  env;
		struct ipc13_cmd_payload  cmd;
		struct ipc13_resp_payload resp;
	};
};

/* Longest UART command line accepted from the operator terminal
 * (including any argument, excluding the newline) -- "3 1" easily fits;
 * sized with headroom for a friendlier future command set. Anything
 * longer than this is truncated by Uart_Cmd_Task's polling loop, not
 * rejected outright. */
#define IPC13_LINE_MAX 32

#endif

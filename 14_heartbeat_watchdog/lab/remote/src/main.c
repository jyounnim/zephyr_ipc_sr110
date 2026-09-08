/*
 * Lab 14: Heartbeat Watchdog (CLIENT, M4)
 *
 * This lab adds no new hardware at all -- it reuses only M4's console
 * UART (already wired since Lab 01, and already proven to work in
 * polling mode in Lab 13) and the mbox channel every lab has used since
 * Lab 01. There is no sensor reading in this lab; the AHT20/BMP280 from
 * Lab 12/13 are simply not touched here, because this lab's subject is
 * the heartbeat/watchdog IPC pattern itself, not sensor telemetry.
 *
 * WHAT M4 DOES: a dedicated thread (`Heartbeat_Task`) sends a tiny
 * beacon message (just an incrementing `seq`) to M55 every
 * HEARTBEAT_PERIOD_MS. That is the entire steady-state behavior.
 *
 * SIMULATING A HANG: a real M4 hang (crash, infinite loop, debugger
 * halt) would obviously also kill this heartbeat, which is exactly what
 * a watchdog is supposed to catch -- but a real hang can't be triggered
 * on demand for a classroom demo, and would also kill the console we'd
 * want to observe the recovery from. So instead, this lab's console
 * command channel (reused from Lab 13's proven `uart_poll_in()` polling
 * design -- interrupt-driven RX is confirmed NOT supported on this
 * UART instance, see Lab 13's troubleshooting doc) lets an operator
 * SUSPEND just the heartbeat send for N seconds while everything else on
 * M4 (console, any future sensor work) keeps running normally. This is a
 * deliberate simplification: it demonstrates M55's timeout-detection
 * logic faithfully (M55 has no way to tell "M4 stopped sending
 * heartbeats" apart from "M4 crashed" -- both just look like silence on
 * the channel), without actually taking down the board.
 *
 * ALL commands in this lab are M4-LOCAL -- unlike Lab 13's commands
 * 1/2/3 (which round-trip to M55), nothing here is relayed over mbox at
 * all, because pausing/resuming/inspecting the heartbeat sender is
 * entirely M4's own business. See handle_command() below.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <string.h>
#include "ipc_common.h"

LOG_MODULE_REGISTER(lab14_client, CONFIG_LOG_DEFAULT_LEVEL);

#define HEARTBEAT_PERIOD_MS 300

#define UART_TASK_STACK_SIZE 1536
#define UART_TASK_PRIORITY   5
#define HB_TASK_STACK_SIZE   1024
#define HB_TASK_PRIORITY     5

#define CMD_LINE_MAX         32
#define UART_POLL_INTERVAL_MS 10

#define PAUSE_SECONDS_MIN 1
#define PAUSE_SECONDS_MAX 60

static struct mbox_dt_spec tx_channel;
static const struct device *console_dev = DEVICE_DT_GET(DT_NODELABEL(ns16550_uart0));

/* Guards pause_until_ms, the only piece of state shared between
 * Uart_Cmd_Task (writer) and Heartbeat_Task (reader). A plain mutex is
 * used rather than an atomic_t because the value is a 64-bit uptime
 * timestamp (k_uptime_get()'s return type), which does not fit in a
 * single atomic_t on this platform. */
static struct k_mutex pause_lock;
static int64_t pause_until_ms;   /* 0 = not paused */

K_THREAD_STACK_DEFINE(uart_task_stack, UART_TASK_STACK_SIZE);
static struct k_thread uart_task_data;
K_THREAD_STACK_DEFINE(hb_task_stack, HB_TASK_STACK_SIZE);
static struct k_thread hb_task_data;

/* ---------------------------------------------------------- heartbeat */

static void heartbeat_task_entry(void *p1, void *p2, void *p3)
{
	uint32_t seq = 0;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	LOG_INF("[Heartbeat_Task] started, period=%dms", HEARTBEAT_PERIOD_MS);

	while (1) {
		bool paused;

		k_mutex_lock(&pause_lock, K_FOREVER);
		if (pause_until_ms != 0 && k_uptime_get() >= pause_until_ms) {
			/* Pause window expired -- auto-clear so a forgotten
			 * "resume" isn't required. */
			pause_until_ms = 0;
		}
		paused = (pause_until_ms != 0);
		k_mutex_unlock(&pause_lock);

		if (!paused) {
			struct ipc14_heartbeat_payload hb = {.seq = ++seq};
			struct mbox_msg mbox_msg = {.data = &hb, .size = sizeof(hb)};

			/* Only Heartbeat_Task ever calls mbox_send_dt() in this
			 * lab -- no other thread sends on tx_channel, so no
			 * mutex is needed around this call (contrast with
			 * Lab 13's tx_lock, which existed because TWO threads
			 * shared one tx_channel there). */
			mbox_send_dt(&tx_channel, &mbox_msg);
		}

		k_msleep(HEARTBEAT_PERIOD_MS);
	}
}

/* ------------------------------------------------- console commands */

/* Handles one command line. Everything here is M4-local -- nothing is
 * ever sent to M55 (contrast with Lab 13, where commands 1/2/3 relayed
 * to M55 and only command 4 was local). Pausing/resuming/inspecting the
 * heartbeat sender is entirely M4's own concern. */
static void handle_command(const char *line)
{
	int sec;

	if (sscanf(line, "1 %d", &sec) == 1) {
		if (sec < PAUSE_SECONDS_MIN || sec > PAUSE_SECONDS_MAX) {
			printk("[M4] pause seconds must be %d..%d\n",
			       PAUSE_SECONDS_MIN, PAUSE_SECONDS_MAX);
			return;
		}
		k_mutex_lock(&pause_lock, K_FOREVER);
		pause_until_ms = k_uptime_get() + (int64_t)sec * 1000;
		k_mutex_unlock(&pause_lock);
		printk("[M4] heartbeat PAUSED for %ds (simulated hang) -- watch M55's TFT\n", sec);
		return;
	}

	if (strcmp(line, "2") == 0) {
		bool was_paused;

		k_mutex_lock(&pause_lock, K_FOREVER);
		was_paused = (pause_until_ms != 0);
		pause_until_ms = 0;
		k_mutex_unlock(&pause_lock);
		printk("[M4] heartbeat RESUMED%s\n", was_paused ? "" : " (was not paused)");
		return;
	}

	if (strcmp(line, "3") == 0) {
		int64_t remaining_ms;

		k_mutex_lock(&pause_lock, K_FOREVER);
		remaining_ms = pause_until_ms ? (pause_until_ms - k_uptime_get()) : 0;
		k_mutex_unlock(&pause_lock);

		if (remaining_ms > 0) {
			printk("[M4] status: PAUSED, %u ms remaining\n", (unsigned int)remaining_ms);
		} else {
			printk("[M4] status: RUNNING (heartbeat active)\n");
		}
		return;
	}

	printk("[M4] unknown command: \"%s\" (try: 1 <1..60>=pause, 2=resume, 3=status)\n", line);
}

/* Polls the console UART for one line at a time, exactly like Lab 13's
 * Uart_Cmd_Task -- see that lab's troubleshooting doc for why polling
 * (uart_poll_in()) is used here instead of an RX interrupt: this UART
 * instance's driver returned -ENOTSUP from uart_irq_callback_set() on
 * real hardware, so interrupt-driven RX simply isn't available. */
static void uart_cmd_task_entry(void *p1, void *p2, void *p3)
{
	char line_buf[CMD_LINE_MAX];
	size_t line_len = 0;
	uint8_t c;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	LOG_INF("[Uart_Cmd_Task] started -- type 1 <1..60> (pause), 2 (resume), "
		"or 3 (status) into this console and press Enter");

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
	LOG_INF("Lab14 Heartbeat Watchdog CLIENT - %s", CONFIG_BOARD_TARGET);

	k_mutex_init(&pause_lock);

	/* Only tx is needed -- M4 never receives anything from M55 in this
	 * lab (contrast with every earlier lab, which always registered an
	 * rx callback too). The heartbeat channel is one-way. */
	tx_channel = (struct mbox_dt_spec)MBOX_DT_SPEC_GET(DT_PATH(mbox_consumer), tx);

	if (!device_is_ready(console_dev)) {
		LOG_ERR("console UART not ready -- command intake disabled");
	}

	k_thread_create(&hb_task_data, hb_task_stack, K_THREAD_STACK_SIZEOF(hb_task_stack),
			heartbeat_task_entry, NULL, NULL, NULL, HB_TASK_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&hb_task_data, "Heartbeat_Task");

	k_thread_create(&uart_task_data, uart_task_stack, K_THREAD_STACK_SIZEOF(uart_task_stack),
			uart_cmd_task_entry, NULL, NULL, NULL, UART_TASK_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&uart_task_data, "Uart_Cmd_Task");

	while (1) {
		k_sleep(K_FOREVER);
	}
	return 0;
}

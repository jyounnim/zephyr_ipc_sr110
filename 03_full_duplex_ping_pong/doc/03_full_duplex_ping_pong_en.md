# Lab 03: Full-Duplex Ping-Pong

A full-duplex IPC lab where M55 and M4 use the tx and rx sides of the same mbox channel pair **simultaneously**, batting a ping-pong message back and forth without pause.

## Learning Objectives

- Understand a full-duplex communication structure in which the mbox tx channel and rx channel are used **at the same time, on the same thread/core**.
- Understand what constraints follow from the fact that the mbox receive callback runs in **ISR (interrupt) context**, and how that shapes code design.
- Implement the "ISR callback → `k_msgq` → worker thread" three-stage pattern in real code, and be able to explain why this pattern is necessary.

## Connection to Previous Labs

- **Lab 01**: The simplest possible mbox communication — a one-way signal from M55 to M4.
- **Lab 02**: Also one-way (M4 → M55), notifying M55 of a button press on M4.
- **Lab 03**: This time both directions are used **simultaneously**. M55 sends a ping; M4 receives it, toggles LED0, and replies with a pong; M55 sends the next ping the instant it receives that pong. This unbroken back-and-forth is the heart of the lab. Where the earlier labs had "one side only sends, the other only receives," from here on **both cores** use the `tx`/`rx` of the same `mbox_consumer` node at the same time.

## Core IPC Concepts

### 1. What Is Full-Duplex IPC?

Full-duplex describes a communication structure in which both ends can transmit and receive **independently and simultaneously** — like a phone call, where both parties can speak and listen at once, as opposed to half-duplex, where a walkie-talkie lets only one side talk at a time.

In this lab, M55 and M4 each:
- send messages to the other side over their own **tx channel**, while
- simultaneously receiving the other side's messages through a callback registered on their own **rx channel**.

The two channels are distinct mbox channels (channels 0/1 of `ipc0`) and are therefore physically independent. So it's perfectly fine for "M55 sending a ping" to overlap in time with "M4 handling follow-up work for the previous pong" — that overlap *is* the essence of full-duplex. The ping-pong exchange in this lab still proceeds in order (a pong only arrives after a ping is sent, and the next ping only goes out after that pong arrives), but the channel structure itself supports simultaneous bidirectional transfer at any time.

### 2. The mbox Callback Must Never Block

This is the single most important concept in this lab. The bottom line, up front:

> **The mbox receive callback (`mbox_rx_callback`) runs in ISR (Interrupt Service Routine) context, and inside it you must never make any blocking call that would put a thread to sleep or require scheduler intervention.**

**Why blocking inside an ISR is forbidden (general RTOS theory)**

An RTOS's interrupt context is a state in which "whatever thread was running got briefly paused and interrupted." There is fundamentally no notion, inside that context, of "putting myself back on the scheduler's queue and yielding the CPU to another thread" — an ISR isn't a thread at all; it's an exception-handling routine that hijacked a thread's execution. But blocking APIs like `k_msleep()`, `k_sem_take(K_FOREVER)`, and `k_mutex_lock()` all assume, internally, that they can "put the current flow of execution to sleep and let the scheduler pick what runs next." Call one of these from inside an ISR, and the kernel ends up in a state with no thread context to fall back to — and most RTOSes (Zephyr included) treat this as a fatal error, hanging the system on the spot or dying to an assertion failure.

**Why this is especially damaging in this project**

Zephyr's default logging mode is deferred: calling `LOG_INF()` and friends only buffers the message, and a separate logging thread later drains that buffer out over UART. But once the kernel hangs inside an ISR, that logging thread never gets scheduled again — meaning even the logs already sitting in the buffer, boot banner included, never make it out. The console ends up showing **nothing at all**, and from the outside it looks as though the board did absolutely nothing. That's exactly what makes this class of bug so much harder to diagnose: the system hangs in a state where you can't even see an error log.

**Why this principle first becomes critical in this lab**

Labs 01/02 were one-way communication, so the receiving side's job amounted to little more than logging the received signal or toggling a single GPIO. Starting with this lab, though, logic appears after the receive callback that **deliberately needs a delay** — "wait a bit, then send the next message" (a 0.5-second interval is used so the ping-pong rhythm is visible). If that delay (`k_msleep()`) were called directly from inside the callback, the system would hang, for exactly the reason explained above.

### 3. The Standard Three-Stage Pattern: ISR → k_msgq → Worker Thread

To avoid this, this lab (and every lab after it) follows the same structure without exception:

1. **mbox receive callback (ISR context)**: Copies the received message into a message queue and returns immediately — nothing more. Only a non-blocking enqueue such as `k_msgq_put(..., K_NO_WAIT)` is used.
2. **`k_msgq`**: Acts as the safe conduit between the ISR and the worker thread. The ISR side never waits (`K_NO_WAIT`); the worker thread side is free to wait as long as it likes for a message (`K_FOREVER`) — since it runs in thread context, blocking there is never a problem.
3. **Worker thread (thread context)**: Pulls the message off the queue and does the actual work. Here, anything is safe — `k_msleep()`, GPIO control, sending the next message, whatever is needed.

This pattern is implemented exactly as described in this lab's M4 (client) code.

```c
static void mbox_rx_callback(const struct device *dev, mbox_channel_id_t channel_id,
			      void *user_data, struct mbox_msg *data)
{
	struct ipc_pingpong_msg msg;
	...
	memcpy(&msg, data->data, sizeof(msg));
	k_msgq_put(&rx_msgq, &msg, K_NO_WAIT);   /* ISR-safe: enqueue and return immediately */
}

static void pong_task_entry(void *p1, void *p2, void *p3)
{
	struct ipc_pingpong_msg msg;
	...
	while (1) {
		if (k_msgq_get(&rx_msgq, &msg, K_FOREVER) != 0) {
			continue;
		}

		gpio_pin_toggle_dt(&led0);   /* Involves I2C access — safe only in thread context */
		LOG_INF("PING received, seq=%u -> LED0 toggled, replying PONG", msg.seq);

		mbox_msg.data = &msg;
		mbox_msg.size = sizeof(msg);
		mbox_send_dt(&tx_channel, &mbox_msg);
	}
}
```

The M55 (host) side uses the identical structure. The one piece of logic new to this lab is deliberately delaying inside the worker thread with `k_msleep(PINGPONG_DELAY_MS)` before sending the next ping.

```c
static void mbox_rx_callback(const struct device *dev, mbox_channel_id_t channel_id,
			      void *user_data, struct mbox_msg *data)
{
	struct ipc_pingpong_msg msg;
	...
	memcpy(&msg, data->data, sizeof(msg));
	/* ISR-safe: no blocking calls here, just enqueue for the worker thread */
	k_msgq_put(&rx_msgq, &msg, K_NO_WAIT);
}

static void pong_task_entry(void *p1, void *p2, void *p3)
{
	struct ipc_pingpong_msg msg;
	...
	while (1) {
		if (k_msgq_get(&rx_msgq, &msg, K_FOREVER) != 0) {
			continue;
		}

		LOG_INF("PONG received, seq=%u", msg.seq);

		/* Pause here (thread context — perfectly fine to block) so the
		 * ping-pong rhythm is easy to watch on the console/LED. */
		k_msleep(PINGPONG_DELAY_MS);
		send_ping(msg.seq + 1);
	}
}
```

In both pieces of code, `mbox_rx_callback()` does nothing beyond a single log line and `k_msgq_put()` — all of the actual "what to do" logic lives in a separate worker thread, `pong_task_entry()` (task name `Pong_Task`). This three-stage structure recurs without exception in every lab from here on; it's the standard skeleton of this curriculum, and it's worth internalizing solidly right now.

> **Note**: This lab does not address whether it's safe to call the mbox send function (`mbox_send_dt()`) directly from inside a callback — the fact that `mbox_send_dt()` on this board's mbox backend can itself block under certain conditions is a separate discovery made later, in Lab 07. The M4 code in this lab already sends its pong from the worker thread regardless, so it's unaffected by that issue either way.

## Architecture

```
M55 (host)                              M4 (client)
----------                              -----------
main()
  send_ping(1)  ----ping(seq)---------->  mbox_rx_callback()
                                             -> k_msgq_put()
                                                    |
                                                    v
                                           Pong_Task
                                             -> LED0 toggle
                                             -> mbox_send_dt(pong)
  mbox_rx_callback()  <----pong(seq)-----------------+
    -> k_msgq_put()
         |
         v
  Pong_Task
    -> k_msleep(500ms)
    -> send_ping(seq+1)  ----ping(seq+1)--------> (repeats)
```

The message struct (`lab/include/ipc_common.h`) is reused as-is for both directions (ping/pong).

```c
/* Shared by both directions: ping (M55->M4) and pong (M4->M55) both use this one struct. */
struct ipc_pingpong_msg {
	uint32_t seq;
};
```

Only a single `seq` number is carried: M4 echoes the received seq straight back in the pong, and M55 adds 1 to the pong's seq before sending the next ping. This makes it easy to confirm from the console logs alone that the round trips are proceeding in order.

## Devicetree Configuration

`ipc0`'s `shared-memory-size` must be set to the same value in both the M55 and M4 overlays. In this lab, both overlays use `0x400`.

```dts
/* lab/boards/sr100_rdk_sr100_m55.overlay */
&ipc0 {
	shared-memory-size = <0x400>;
};
```

```dts
/* lab/remote/boards/sr100_rdk_sr100_m4.overlay */
&ipc0 {
	shared-memory-size = <0x400>;
};
```

M55 and M4 are also wired to physically share the same I2C1 bus (SCL/SDA) on this board. To ensure only M4 drives `gpio_exp0` (PCA6416A), which LED0/LED1 hang off of, the M55 overlay explicitly disables `i2c1` and its child nodes.

```dts
&i2c1 {
	status = "disabled";
};

&gpio_exp0 {
	status = "disabled";
};

&ov02c10 {
	status = "disabled";
};
```

Disabling only the parent node (`i2c1`) is not enough — a child node whose `status` is not explicitly set keeps the default value `"okay"`, so the driver still treats it as an "active instance" and tries to pull it into the build. In that case `DT_BUS()` cannot find a handle for the now-disabled `i2c1`, causing a build error, so the child nodes must be explicitly disabled as well.

## How to Build

```bash
# 1) M4 (remote)
west build -p always -b sr100_rdk/sr100/m4 ./zephyr_ipc_sr110/03_full_duplex_ping_pong/lab/remote -d m4

# 2) M55 (host, bundling the M4 binary in via M4_BUILD)
west build -p always -b sr100_rdk/sr100/m55 ./zephyr_ipc_sr110/03_full_duplex_ping_pong/lab -d m55 \
    -DCONFIG_SR100_RELEASE_M4_RESET=y -DM4_BUILD="../m4"
```

> **Note**: `M4_BUILD` is a relative path from the M55 build directory (`m55/`). If `m4/` and `m55/` are sibling directories under the workspace root, `../m4` is correct.

## Running and Verifying the Result

Flash the board and watch both cores' consoles together:

- **M4**: LED0 keeps toggling on a roughly 1-second cycle (0.5 s round trip × 2).
- **Both M55 and M4 consoles**: PING/PONG seq numbers keep incrementing and printing without pause.

```
[M55] PING sent, seq=1
[M4 ] PING received, seq=1 -> LED0 toggled, replying PONG
[M55] PONG received, seq=1
[M55] PING sent, seq=2
[M4 ] PING received, seq=2 -> LED0 toggled, replying PONG
[M55] PONG received, seq=2
...
```

If both logs keep flowing without interruption and LED0 keeps toggling at a steady cadence, the lab is working correctly.

## Key Takeaways

- Full-duplex IPC is a structure in which both cores can use two channels (tx/rx) simultaneously and independently.
- **The mbox receive callback runs in ISR context, so it must never make any blocking call.** Blocking inside an ISR gives the scheduler nothing to fall back to and hangs the kernel — and because of Zephyr's deferred logging, even logs already buffered before the hang never make it to the screen.
- The standard pattern that avoids this is **ISR callback → `k_msgq_put(K_NO_WAIT)` → actual processing in a worker thread via `k_msgq_get(K_FOREVER)`**. This three-stage structure is the basic skeleton of this curriculum, repeated in every lab that follows.

## Next Lab

Lab 04 moves on to a data-passing lab built around a struct payload: M55 periodically sends a counter value, and M4 receives it and blinks an LED a corresponding number of times.

---

Ran into a problem? → See [`03_full_duplex_ping_pong_troubleshooting_en.md`](./03_full_duplex_ping_pong_troubleshooting_en.md)

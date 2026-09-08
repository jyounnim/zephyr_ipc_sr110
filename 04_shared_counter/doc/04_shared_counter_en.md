# Lab 04: Shared Counter (M55 → M4)

In this lab, M55 increments a counter once per second and sends the value to M4, which "displays" it by blinking an LED that many times. Building on the mbox signaling you learned in Labs 01–03, from this lab onward we exchange messages that carry **real data (a payload)**.

## Learning Objectives

- Learn how to carry a struct payload inside an mbox message.
- Understand how keeping "shared state" consistent across cores differs from actually sharing memory.
- See, through code that genuinely needs to block (LED blinking), why received data must be processed on a separate worker thread rather than directly in the ISR callback.
- Understand how the devicetree `shared-memory-size` property relates to the mbox message size.

## Connection to Previous Labs

Labs 01–03 only exchanged "signals" over mbox — the payload was zero-sized or trivially small, so the message itself carried no meaningful data, only the fact that "an event occurred" (an M55→M4 ping, an M4→M55 pong, and so on).

Starting with Lab 04, we send **meaningful data** — a `uint32_t counter` value — inside an mbox message for the first time. M55 increments its counter by 1 every second and transmits it to M4, and M4 "displays" the received value by blinking LED0 `counter % 5 + 1` times, letting you verify the value visually without a screen.

This lab also puts to the test a principle established in Lab 03 — **an mbox receive callback (running in ISR context) must never block** — by introducing, for the first time, processing that genuinely requires blocking (toggling an LED multiple times requires spacing the toggles out with `k_msleep()`), so you can feel firsthand why that principle matters.

## Core IPC Concept: "Shared State," Not "Shared Memory"

Because the lab is titled "Shared Counter," it's easy to misread what's going on: **M55 and M4 are not physically sharing a `counter` variable.**

### Message Passing vs. True Shared Memory

It's worth distinguishing two approaches clearly.

1. **True shared memory**: both cores map the same physical address, so when one side writes a value there, the other can read it back with no explicit notification (or by polling). Because both cores can touch the same variable at the same time, synchronization primitives such as mutexes or atomic operations are mandatory.
2. **Message passing** — what this lab actually uses: M55 increments its own local variable `msg.counter`, then **copies the entire value into a message** and "sends" it to M4 over mbox. M4 receives that message and stores it into **its own local copy** (`struct ipc_counter_msg msg`, a stack variable inside `blink_task_entry()`).

In other words, saying "the counter is shared" is true only in a **logical** sense — it means both cores "know" the same value, not that they reference the same memory address. In practice, it's closer to M55 taking a snapshot of its counter every second and mailing it to M4. Even if M55 has already advanced to the next value while M4 is still processing the message it received, M4's own copy is unaffected.

The devicetree property `&ipc0 { shared-memory-size = <0x400>; };` uses the word "shared memory" in its name, but it is not **a variable space the application reads and writes directly — it's an internal transfer buffer (MTU) that the mbox driver uses to carry messages between cores.** It's fully encapsulated behind the `mbox_send_dt()` / receive-callback API boundary, so application code never needs to think about it — all you see are function calls to send and receive messages.

### Which Is Why There's No Mutex in This Lab

Because message passing is used, **the two cores never touch the same memory location at the same time**, so no cross-core synchronization primitive — a mutex, a semaphore — is needed to avoid a race condition. The `counter` value always exists as one of exactly two things — "M55's local variable" or "the local copy M4 just received" — and the only point where it crosses that boundary is the single act of sending an mbox message.

That said, this lab does introduce one data structure for synchronization — not **between cores**, but **between threads within M4**: `K_MSGQ_DEFINE(rx_msgq, ...)`. More on that below.

## Architecture and Code Walkthrough

### Message Definition (`lab/include/ipc_common.h`)

```c
struct ipc_counter_msg {
	uint32_t counter;
};
```

This is the step forward from earlier labs — we define a struct that carries a payload, and exchange the whole struct as the mbox message.

### M55 (host) — `lab/src/main.c`

```c
struct mbox_dt_spec tx_channel;
struct ipc_counter_msg msg = {.counter = 0};
struct mbox_msg mbox_msg;

tx_channel = (struct mbox_dt_spec)MBOX_DT_SPEC_GET(DT_PATH(mbox_consumer), tx);

while (1) {
	msg.counter++;

	mbox_msg.data = &msg;
	mbox_msg.size = sizeof(msg);
	ret = mbox_send_dt(&tx_channel, &mbox_msg);
	...
	k_msleep(TICK_PERIOD_MS);
}
```

M55 is a **unidirectional (M55 → M4) lab** with no mbox receive callback at all. Since the `while` loop inside `main()` already runs in thread context, there's nothing wrong with calling `k_msleep(1000)` to wait one second and then calling `mbox_send_dt()`. Lab 03's "callbacks must not block" rule simply doesn't apply here, because this code has no callback to begin with.

`mbox_msg.data = &msg; mbox_msg.size = sizeof(msg);` is the heart of this lab — you point at the payload's starting address and give its size, and when that's passed to `mbox_send_dt()`, the driver copies the contents into M4's receive buffer.

### M4 (client) — `lab/remote/src/main.c`

The M4 side follows a three-stage structure: **ISR callback → message queue (`k_msgq`) → worker thread**.

```c
K_MSGQ_DEFINE(rx_msgq, sizeof(struct ipc_counter_msg), 4, 4);

static void mbox_rx_callback(const struct device *dev, mbox_channel_id_t channel_id,
			      void *user_data, struct mbox_msg *data)
{
	struct ipc_counter_msg msg;

	if (data->size < sizeof(msg)) {
		return;
	}
	memcpy(&msg, data->data, sizeof(msg));
	k_msgq_put(&rx_msgq, &msg, K_NO_WAIT);
}
```

`mbox_rx_callback()` runs in **ISR context**. It does exactly two things: copies the received data into a local variable, and pushes that copy onto the message queue (using `K_NO_WAIT`, so it returns immediately without blocking even if the queue is full). It does **absolutely no** work to figure out how many times to blink the LED or to actually toggle a GPIO.

```c
static void blink_task_entry(void *p1, void *p2, void *p3)
{
	struct ipc_counter_msg msg;
	uint32_t blink_count;

	while (1) {
		if (k_msgq_get(&rx_msgq, &msg, K_FOREVER) != 0) {
			continue;
		}

		blink_count = (msg.counter % 5U) + 1U;
		LOG_INF("counter=%u received -> blinking LED0 %u times", msg.counter, blink_count);

		for (uint32_t i = 0; i < blink_count; i++) {
			gpio_pin_set_dt(&led0, 1);
			k_msleep(BLINK_ON_MS);
			gpio_pin_set_dt(&led0, 0);
			k_msleep(BLINK_OFF_MS);
		}
	}
}
```

The actual value interpretation (`counter % 5 + 1`) and the LED blinking (a blocking operation that repeatedly calls `k_msleep()`) are handled on a dedicated worker thread called `Blink_Task`, created separately for this purpose. This thread sleeps on `k_msgq_get(&rx_msgq, &msg, K_FOREVER)` until a new message arrives on the queue, then wakes up and processes it.

**Why this has to be split up this way**: blinking the LED `blink_count` times means repeatedly turning it on (150 ms) and off (150 ms), and that "wait 150 ms" is unmistakably a blocking operation. If this logic had been left inside `mbox_rx_callback()`, which runs in ISR context, the interrupt handler would have occupied the kernel for up to nearly 1.5 seconds (5 iterations × 300 ms), blocking every other interrupt and effectively freezing the system. This is the same class of bug as in Lab 03, where putting `k_msleep()` directly inside an mbox callback froze the kernel outright. Separating "receiving" from "processing" into different execution contexts via `k_msgq` is the standard pattern that avoids this problem at its root.

## Devicetree Configuration Walkthrough

### `lab/boards/sr100_rdk_sr100_m55.overlay` (M55)

```dts
&ipc0 {
	shared-memory-size = <0x400>;
};

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

- `shared-memory-size = <0x400>;`: the size of the internal transfer buffer (MTU) used to carry mbox messages. This value **must match exactly** in both the M4 and M55 overlays. The message this lab actually sends (`struct ipc_counter_msg`, 4 bytes) is tiny compared to this limit.
- Disabling `&i2c1`, `&gpio_exp0`, and `&ov02c10` isn't specific to this lab — it's a **setting shared across the entire lab series**. M4 and M55 physically share the same I2C1 bus (the GPIO expander `gpio_exp0`, which LED0/LED1 and the button hang off of, lives on this bus), and if both cores tried to initialize this bus at the same time they would collide. So M55 explicitly disables this bus and its child devices, leaving M4 as the sole owner of the LEDs and button.

### `lab/remote/boards/sr100_rdk_sr100_m4.overlay` (M4)

```dts
&ipc0 {
	shared-memory-size = <0x400>;
};
```

On the M4 side, all that's needed is to match the `shared-memory-size` value with M55's. LED0 is already enabled by default in M4's base devicetree, so no additional overlay entry is required for it.

## How to Build

```bash
# 1) Build M4 (remote) first
west build -p always -b sr100_rdk/sr100/m4 ./zephyr_ipc_sr110/04_shared_counter/lab/remote -d m4

# 2) Build M55 (host, embedding the M4 binary via M4_BUILD)
west build -p always -b sr100_rdk/sr100/m55 ./zephyr_ipc_sr110/04_shared_counter/lab -d m55 \
    -DCONFIG_SR100_RELEASE_M4_RESET=y -DM4_BUILD="../m4"
```

> **Note**: `M4_BUILD` is a relative path resolved against the M55 build directory (`m55/`). If `m4/` and `m55/` are sibling directories at the workspace root, `../m4` is correct.

## Running and Verifying the Result

When everything works correctly, the two cores' console output looks like this:

- **M55 console**: `counter=N sent` is printed every second (N keeps increasing from 1).
- **M4 console**: `counter=N received -> blinking LED0 M times` is printed, and LED0 actually blinks briefly `M = N % 5 + 1` times. So it repeats as: N=1 → 2 blinks, N=5 → 1 blink, N=6 → 2 blinks, and so on.

## Summary

- Lab 04 is the first lab to send **meaningful data** (a struct payload) inside an mbox message.
- "Shared state" means M55 and M4 logically share the same value through message passing — it is different from physically sharing the same memory variable, which is why no cross-core synchronization primitive (such as a mutex) is needed.
- The devicetree's `shared-memory-size` is not a resource the application manipulates directly — it's an internal transfer buffer (MTU) belonging to the mbox driver.
- On the M4 side, the chain ISR callback (`mbox_rx_callback`) → message queue (`rx_msgq`) → worker thread (`Blink_Task`) safely performs processing that genuinely needs to block (LED blinking) outside of ISR context.

## Next Lab Preview

Lab 05 works in the opposite direction — M4 counts button presses and sends the count to M55. You'll practice the same "carry a counter as a message" pattern, but flipped to the M4 → M55 direction.

---

Ran into a problem? → See [`04_shared_counter_troubleshooting_en.md`](./04_shared_counter_troubleshooting_en.md)

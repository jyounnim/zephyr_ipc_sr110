# Lab 02: Button Pong — Forwarding an M4-Detected Button Press to the M55

This lab has the M4 handle a real GPIO (button) interrupt and forward the resulting "event" to the M55 over mbox IPC. Going beyond Lab 01's simple ping-pong, this lab covers one of the most common patterns in real embedded systems: **"the core that owns a device detects an event, and notifies the other core."**

## Learning Objectives

By the end of this lab, you will be able to:

- Use Zephyr's **Input subsystem** (`zephyr/input/input.h`) to receive `gpio-keys`-based button input through a callback.
- Safely handle an event that originates in interrupt/callback context using the **message queue (`k_msgq`) + worker thread** pattern.
- Design a structure that forwards an event from a peripheral owned by only one core (an I2C GPIO expander on the M4) to another core (the M55) over mbox.
- Judge why and when a driver-specific workaround option such as `polling-mode` is needed in devicetree.
- Distinguish, when an mbox callback runs in ISR context, which work can be done directly inside the callback and which work must be deferred to a worker thread.

## Connection to the Previous Lab

Lab 01 (`Hello IPC`) covered a purely software-driven IPC ping-pong, where the M55 sent "ping" first and the M4 replied with "pong". The trigger for sending a message lived in the code itself (a timer or a loop), and no real hardware event was involved.

Lab 02 extends this in two ways:

1. **The trigger is a real hardware interrupt (a button), not software.** The IPC message is now driven not by "a scheduled time to send" but by "an asynchronous event whose timing is unknown."
2. **The direction is now fixed as M4 → M55.** On this board, both the button (SW8) and the LED sit behind an I2C GPIO expander (`gpio_exp0`, PCAL6416A), and because the M4 and M55 physically share the same I2C1 bus, `gpio_exp0` is owned only by the M4 in this lab and in every other lab (see the I2C1 bus-sharing explanation in the Lab 01 document for the full background). In other words, only the M4 can touch either the button or the LED, and the M55 acts purely as an observer that gets notified of "what happened."

In short, if Lab 01 was about confirming "is the IPC channel alive," Lab 02 is the more realistic scenario of "forwarding a real device event to another core."

## Core IPC Concepts

### 1. M4 → M55: The "Forward a Device Event to the Host" Pattern

This lab's architecture illustrates the common asymmetric structure where **a sensor/peripheral core (M4) detects an event and an application core (M55) consumes that event**.

```
        [ SW8 button ]
              │  (I2C, gpio_exp0)
              ▼
      ┌───────────────┐
      │   M4 (CLIENT) │  Detects button input + toggles LED1 locally
      │               │
      │  Input cb     │
      │      │        │
      │      ▼        │
      │  k_msgq       │
      │      │        │
      │      ▼        │
      │ Button_Task   │──── mbox_send_dt() ────┐
      └───────────────┘                        │
                                                ▼
                                        ┌───────────────┐
                                        │  M55 (HOST)   │
                                        │  Just logs it │
                                        │  in the mbox  │
                                        │  callback     │
                                        └───────────────┘
```

The M4 is the only core on this board that owns `gpio_exp0` (LED0/LED1/button), so it is the first to react when the button is pressed (toggling LED1 for immediate visual feedback). It then sends a minimal mbox message containing only "which event number this is" to the M55. The M55 does nothing with this message beyond logging it to the console, **because it cannot turn on LED1 itself** — LED1 also sits behind that same `gpio_exp0`, and I2C1 is disabled on the M55 side, making it physically unreachable.

This structure is extremely common in practice. For example, an M4 might handle low-level peripherals such as sensors or buttons, while an M55 (application processor) receives those events and handles UI updates, logging, or higher-level logic. Keep in mind that **who owns the hardware determines the direction and payload design of the IPC messages**.

### 2. ISR Safety When a GPIO Interrupt Callback and an mbox Callback Both Run

The M4-side code in this lab has two layers of code that run in interrupt/callback context:

- `button_input_cb()`, registered via Zephyr's Input subsystem with `INPUT_CALLBACK_DEFINE()`, is invoked in **near-interrupt context** (specifically, either the input driver's workqueue or an ISR, depending on the driver).
- The mbox receive callback (already covered in Lab 01) also runs in **ISR context**.

Both cases share the same constraint: **blocking-capable work (an I2C transaction, `k_sleep`, waiting on a mutex, etc.) must never be performed directly in ISR/callback context.** If `button_input_cb()` called `gpio_pin_toggle_dt(&led1)` (an I2C bus transaction) and `mbox_send_dt()` directly, the kernel could assert — or at minimum harm overall system responsiveness — depending on the context of the driver that invoked the callback.

This is why `button_input_cb()` does no real work at all: it simply pushes the fact that "a press event occurred" onto a queue with `k_msgq_put()` (with `K_NO_WAIT`, so the callback never blocks even if the queue is full) and returns immediately. The actual LED toggle and mbox send are handled by a separate `Button_Task` worker thread, which pulls the event off the queue and processes it in thread context. This reuses the exact "ISR/callback → `k_msgq` → worker thread" pattern already introduced in Lab 01 — showing that even with two layers of callbacks (the input callback plus the eventual mbox callback), a single consistent pattern keeps both safe.

By contrast, the M55-side `mbox_rx_callback()` calls `LOG_INF()` directly inside the callback and returns. Logging is (generally, configuration permitting) a low-blocking-risk, short-lived operation, making it a good contrasting example of work that's fine to handle directly in the callback without deferring it to a worker thread. Between the M4 side and the M55 side, this shows that **"running in ISR/near-ISR context" does not automatically mean "must be deferred to a worker thread" — the deciding factor is whether the work performed inside the callback can block (I2C, mutexes, long computation, etc.).**

### 3. Why `polling-mode` Is Needed — a Hardware Constraint of gpio_exp0

The first time you build and run the M4 image, the boot log may show the following error (the final code already includes the fix, so building now will not reproduce it).

```
<err> gpio_keys: interrupt configuration failed: -134
<err> gpio_keys: Pin 0 interrupt configuration failed: -134
```

`-134` is Zephyr's `-ENOTSUP`. The root cause lies in this board's hardware wiring.

- `user_button` sits behind `gpio_exp0` (PCAL6416A, an I2C-attached GPIO expander).
- For `gpio_exp0` to be used in interrupt mode, the expander chip's INT pin must be wired to an MCU GPIO, which devicetree expresses via the `int-gpios` property.
- However, **the `gpio_exp0` node in the M4-side base devicetree has no `int-gpios` defined.** (The M55-side definition does have `int-gpios = <&gpioa 3 GPIO_ACTIVE_LOW>;`, but since the M55 disables I2C1 in this lab — as it does in every lab — that definition is moot anyway.)
- The `gpio_pcal64xxa.c` driver is written so that `pin_interrupt_configure()` always returns `-ENOTSUP` for any instance that lacks `int-gpios`.

In other words, there is a **hardware constraint on this board's M4 side: no pin behind `gpio_exp0` can receive events via interrupts.** Since the default `gpio-keys` driver operates in interrupt mode, it runs straight into this constraint.

Zephyr's `zephyr,gpio-keys` binding (`zephyr/dts/bindings/input/gpio-keys.yaml`) provides a `polling-mode` boolean property for exactly this situation. Enabling it makes the driver skip interrupt configuration entirely and instead read the pin state periodically on a timer (at the default `debounce-interval-ms` of 30 ms) to determine press/release. This has been added to the M4 overlay as follows:

```dts
&buttons {
	polling-mode;
};
```

**The takeaway from this lab**: forgetting a single devicetree property doesn't necessarily break the feature itself — it can instead determine "whether that feature can be used with interrupts," which is dictated by the hardware wiring. And Zephyr drivers often provide an option to work around exactly this kind of constraint (here, `polling-mode`). Reading the error code (`-ENOTSUP`) together with the driver source lets you find both "why it doesn't work" and "how to work around it."

## Architecture and Code Walkthrough

### Overall Flow

```
User presses the SW8 button
        │
        ▼ (gpio_exp0, I2C, polled every 30 ms)
Zephyr gpio-keys driver → Input subsystem event fires
        │
        ▼
button_input_cb()  ── callback registered via INPUT_CALLBACK_DEFINE() (near-ISR context)
        │  filters for press (value==1) only, then k_msgq_put(K_NO_WAIT)
        ▼
press_msgq (k_msgq, 4 entries)
        │
        ▼
Button_Task (worker thread, priority 5)
        │  1) increments g_press_count
        │  2) gpio_pin_toggle_dt(&led1)  — I2C, safe here since it's thread context
        │  3) mbox_send_dt(&tx_channel, ...)  — sends struct ipc_button_evt { seq }
        ▼
                    (mbox, IPC)
        ▼
mbox_rx_callback()  ── M55 side, ISR context
        │  memcpy's the event, then logs immediately with LOG_INF (no blocking work, so no worker thread needed)
        ▼
Console prints "Button event received, seq=N"
```

### Shared Header — `lab/include/ipc_common.h`

```c
struct ipc_button_evt {
	uint32_t seq;
};
```

Like Lab 01's ping/pong payload, this lab's payload is kept minimal. It contains only `seq`, which counts which press this is. The fact that "the button was pressed" is already conveyed by the mbox message's arrival itself, so the payload only needs to carry supporting information (the sequence number).

### M4 (CLIENT) — `lab/remote/src/main.c`

**Registering and filtering the input callback**

```c
static void button_input_cb(struct input_event *evt, void *user_data)
{
	uint8_t dummy = 1;

	ARG_UNUSED(user_data);

	/* Only handle press (1), ignore release (0) */
	if (evt->code != INPUT_KEY_0 || evt->value != 1) {
		return;
	}

	k_msgq_put(&press_msgq, &dummy, K_NO_WAIT);
}
INPUT_CALLBACK_DEFINE(NULL, button_input_cb, NULL);
```

When the first argument of `INPUT_CALLBACK_DEFINE(NULL, ...)` is `NULL`, this callback receives events from every input device in the system (not a problem here, since this lab has only the single `buttons` node). `evt->code` is the value specified as `zephyr,code = <INPUT_KEY_0>;` in devicetree, and `evt->value` is 1 for a press and 0 for a release. Since this lab has no interest in release events, only presses are filtered through to the queue. The value placed on the queue is a meaningless dummy (`dummy = 1`) — the signal is the mere fact that a message exists on the queue.

**The actual processing in the worker thread**

```c
while (1) {
	if (k_msgq_get(&press_msgq, &dummy, K_FOREVER) != 0) {
		continue;
	}

	g_press_count++;

	/* Visual feedback: toggle local LED1 (I2C, safe here since we're in thread context) */
	gpio_pin_toggle_dt(&led1);

	/* Send the event to M55 */
	out_evt.seq = g_press_count;
	mbox_msg.data = &out_evt;
	mbox_msg.size = sizeof(out_evt);

	ret = mbox_send_dt(&tx_channel, &mbox_msg);
	...
}
```

`Button_Task` blocks with `K_FOREVER` while the queue is empty, and once an event arrives, it performs, in order: (1) increment the sequence counter, (2) toggle LED1 (I2C — safe to block here since we're in thread context), and (3) send over mbox. This reuses the same `mbox_dt_spec` / `mbox_send_dt()` API already learned in Lab 01.

### M55 (HOST) — `lab/src/main.c`

```c
static void mbox_rx_callback(const struct device *dev, mbox_channel_id_t channel_id,
			      void *user_data, struct mbox_msg *data)
{
	struct ipc_button_evt evt;

	...
	if (data->size < sizeof(evt)) {
		return;
	}
	memcpy(&evt, data->data, sizeof(evt));

	LOG_INF("Button event received, seq=%u", evt.seq);
}
```

Since the M55 cannot turn on LED1 itself (hardware ownership belongs to the M4), all it can do is log the event. As the code comment notes, work this lightweight is fine to handle directly in callback (ISR) context, with no need for a separate worker thread or message queue. This is a direct illustration of the decision criterion described in "Core IPC Concepts" above — whether the work inside the callback can block.

## Devicetree Configuration

### M4 Overlay — `lab/remote/boards/sr100_rdk_sr100_m4.overlay`

```dts
/* ipc0 shared-memory-size must match the M55 overlay in boards/ exactly. */
&ipc0 {
	shared-memory-size = <0x400>;
};

&buttons {
	polling-mode;
};
```

- `ipc0`'s `shared-memory-size` is the size of the shared-memory region used by mbox, and it must be identical in both the M4 and M55 overlays (as covered in Lab 01).
- `&buttons { polling-mode; };` is, as explained above, the setting that works around the hardware constraint that prevents interrupts from being configured on `gpio_exp0` due to the missing `int-gpios`.

### M55 Overlay — `lab/boards/sr100_rdk_sr100_m55.overlay`

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

The M4 and M55 physically share the same I2C1 bus (SCL/SDA pins). If both cores tried to initialize the same bus simultaneously, a conflict could occur at boot, so the M55 side disables `i2c1` entirely, leaving the bus solely to the M4. Disabling the parent bus (`i2c1`) alone is not enough, because child nodes (`gpio_exp0`, `ov02c10`) default to `status = "okay"` unless their own `status` is set explicitly. In that case the build system would still treat them as "active instances" and try to compile their drivers, and `DT_BUS()` would fail to find the (disabled) `i2c1` device handle, causing a build error. That's why the child nodes are also explicitly set to `disabled`. (This was first discovered during Lab 01 hardware testing and has since been applied uniformly across every lab.)

Thanks to this overlay, the M55-side code contains no code at all that accesses `gpio_exp0`, the LED, or the button — its only job is receiving mbox messages and logging them.

## Building

This lab consists of two independent images: the M4 (remote, CLIENT) and the M55 (host, HOST). Because the M55 image embeds the M4 binary, **the M4 must be built first**.

```bash
# 1) M4 (remote)
west build -p always -b sr100_rdk/sr100/m4 ./zephyr_ipc_sr110/02_button_pong/lab/remote -d m4

# 2) M55 (host, embeds the M4 binary via M4_BUILD)
west build -p always -b sr100_rdk/sr100/m55 ./zephyr_ipc_sr110/02_button_pong/lab -d m55 \
    -DCONFIG_SR100_RELEASE_M4_RESET=y -DM4_BUILD="../m4"
```

> **Note**: `M4_BUILD` is a path relative to the M55 build directory (`m55/`). If `m4/` and `m55/` are sibling directories under the workspace root, `../m4` is correct.

## Running and Verifying Results

After flashing and booting both images, you should observe the following behavior.

1. Each time you press the SW8 button, the following log appears on the M4 console, and LED1 toggles at the same time.
   ```
   [Button_Task] button pressed, seq=1 -> sent to M55
   ```
2. Shortly after, the following log appears on the M55 console.
   ```
   Button event received, seq=1
   ```
3. As you keep pressing the button, `seq` increments by 1 each time, and the `seq` values in the M4 and M55 logs should always match.

Immediately after boot, both sides print a "ready"-style log (`M4 ready, waiting for button presses.` / `Waiting for button events from M4.`), after which both cores remain idle until a button press occurs.

## Summary

- Lab 02 shifted the trigger from **a software-generated event (Lab 01) to a real hardware event (a button)**, demonstrating mbox IPC being used for genuine device-event notification.
- Hardware ownership (on this board, `gpio_exp0` = M4) determines the direction of the IPC message (M4 → M55) and the division of roles (M4 as actuator + sensor, M55 as observer).
- In interrupt/callback context (whether an Input callback or an mbox callback), the decision to handle work immediately (the M55's logging) versus deferring it to a worker thread (the M4's I2C LED toggle + mbox send) must be based on **whether the work can block**.
- Hardware wiring information in devicetree, such as the presence or absence of `int-gpios`, determines the driver's operating mode (interrupt vs. `polling-mode`), and reading the error code (`-ENOTSUP`) together with the binding documentation reveals the workaround.

## Coming Up in the Next Lab

Lab 03 will extend this lab's "one-way M4 → M55 event notification" to cover more complex payloads and two-way interaction. The "ISR/callback → message queue → worker thread" pattern and the devicetree overlay techniques learned in Lab 02 will keep being reused throughout every lab that follows, so if anything here is still unclear, it's worth rereading this document before moving on.

---

Ran into a problem? → See [`02_button_pong_troubleshooting_en.md`](./02_button_pong_troubleshooting_en.md)

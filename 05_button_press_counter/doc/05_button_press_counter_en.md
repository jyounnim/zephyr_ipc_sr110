# Lab 05: Button Press Counter (M4 → M55)

In this lab, every time `user_button` is pressed, the M4 computes a **running total** and sends it to the M55 over mbox. The M55 simply logs the value it receives.

## Learning Objectives

- Understand the pattern of converting a repeating event (a button press) into an **accumulated state value (running total)** before passing it between cores, instead of forwarding the raw event every time.
- Understand which hardware constraint requires the `polling-mode` property of `zephyr,gpio-keys`, and which devicetree node it must be applied to.
- Be able to explain why it is useful to split "counting events" and "reacting to events" across two cores.

## Connection to the Previous Lab

In Lab 02 (Button Pong), every button press caused the M4 to send a single **sequence number (seq)** to the M55, and the M55 only logged the fact that the event had arrived. In other words, the M4 was counting "which event number this is," but that number itself was not the point of the lab.

Lab 05 takes this a step further and makes **the count value itself the purpose of the message**. Every time the button is pressed, the M4 increments an internal counter (`g_total_presses`) and sends its latest value as-is to the M55. The M55 receives not the fact that an event occurred, but the **state**: "how many times has it been pressed in total, so far." LEDs are not covered in this lab (Lab 02 already covered the button+LED combination).

## Core IPC Concepts

### 1. Turning events into state before sending

There are broadly two ways to convey events between two cores.

- **Send the event itself**: just send the fact that "the button was pressed just now." The receiver has to count on its own or react to every single event.
- **Send the accumulated state**: send the latest state value, "it has been pressed N times in total so far." The receiver can simply trust that value as-is, with no computation of its own.

Lab 05 takes the latter approach. `struct ipc_press_count_msg` has only a single field, `uint32_t total_presses`, and every message carries a complete snapshot of "the total so far." This approach has two advantages.

1. **The receiver's logic becomes simpler.** The M55 doesn't need to remember how many messages it has received before. A single, latest message is enough to know the current state precisely (a property close to idempotency).
2. **It is reasonably robust against message loss.** If a single mbox message were somehow dropped for some reason (the mbox hardware used in this lab is a reliable channel by default, but from the perspective of general IPC design), the very next message would re-synchronize the state to the correct total as of that point. By contrast, if the design only ever sent "+1 per event," losing a single message would permanently desynchronize the counts on the two cores.

This pattern of "send the entire latest-state snapshot" is a basic design principle that recurs repeatedly in later labs (e.g., shared counters, telemetry).

### 2. Why `polling-mode` must be on the parent node (`&buttons`)

On this board, `user_button` sits behind `gpio_exp0` (a PCAL6416A, a GPIO expander chip connected via I2C). The problem is that **the `gpio_exp0` definition in the M4-side base devicetree has no `int-gpios` property** (the M55-side definition does have one, but it is irrelevant here because the M55 disables the i2c1 bus itself in this lab — see item 3 below).

The `gpio_pcal64xxa.c` driver is written so that `pin_interrupt_configure()` always returns `-ENOTSUP` (-134) for any instance where `int-gpios` is not wired up. In other words, this expander chip has no physical pin connection to notify the M4 of an interrupt in the first place. As a result, Zephyr's default `gpio-keys` behavior — "call the callback when an interrupt arrives" — simply cannot hold on this hardware configuration.

Zephyr provides the `zephyr,gpio-keys` binding with a boolean property called `polling-mode` for exactly this situation. Enabling it makes a kernel timer periodically read the pin state (at the default `debounce-interval-ms` interval of 30 ms) to detect button presses, instead of waiting for an interrupt.

What matters here is that **this property must be on the `gpio-keys` parent node (`&buttons`), and must not be placed on the individual button child node (`&user_button`)**. `polling-mode` is declared in the devicetree binding (`gpio-keys.yaml`) as a parent-node-level property, so attaching it to a child node produces a binding validation error. This lab's M4 overlay (`lab/remote/boards/sr100_rdk_sr100_m4.overlay`) applies it as follows.

```dts
&buttons {
	polling-mode;
};
```

### 3. Division of labor: M4 takes on polling, M55 only receives the result

Polling mode consumes more resources than the interrupt-based approach, since the CPU has to wake up periodically to check the pin state (though at this polling rate the overhead is not significant, this holds as a general principle). In this lab's structure, that burden falls entirely on **the M4**. The M4 is responsible for everything: polling the button through the input subsystem, detecting the press, incrementing the count, and sending it over mbox.

The **M55, on the other hand, does no polling and makes no decisions at all.** All the M55 does is register a single mbox callback and log the value when a message arrives. In short, the messy, repetitive work of "low-level hardware polling and debouncing" is owned by the M4, which owns the local I/O directly, while the M55 only consumes the already-refined result (the current count).

This kind of division of labor is common in real AMP (Asymmetric Multiprocessing) systems. Low-level sensor/input polling, or work where real-time responsiveness matters, is assigned to the core that is physically closer to (or lower-power relative to) that hardware, while higher-level application logic, UI, and logging are handled by a different core — spreading the load and responsibility across the system.

## Architecture and Code Walkthrough

### Message Definition (`lab/include/ipc_common.h`)

```c
struct ipc_press_count_msg {
	uint32_t total_presses;
};
```

A simple single-field structure. This one structure represents the entire state of "how many times has it been pressed in total, so far."

### M4 (CLIENT, `lab/remote/src/main.c`)

The M4 side is structured in three parts.

1. **Input callback (`button_input_cb`)**: When the Zephyr Input subsystem detects a button press (via polling), this callback is invoked in workqueue context. Only when the event is `INPUT_KEY_0` and `value == 1` (pressed, not released) does it push a single dummy byte onto a message queue (`press_msgq`) and return immediately. It does not increment the count or send the mbox message itself here.
2. **`Send_Task` worker thread (`send_task_entry`)**: A dedicated thread blocks on `press_msgq` (`K_FOREVER`), and when an item arrives, it increments `g_total_presses`, packs the value into a `struct ipc_press_count_msg`, and sends it to the M55 with `mbox_send_dt()`.
3. **`main()`**: Initializes the mbox tx channel, spawns the `Send_Task` thread, and then leaves everything else to that thread while it goes to sleep.

The reason the input callback and the actual send logic are split into separate threads is the same principle established in Lab 03 — **never make blocking calls (including mbox sends) from callback/ISR context**. `button_input_cb` only enqueues and returns immediately, so it never blocks the input subsystem's workqueue. The mbox send, which can potentially take time, is performed in a dedicated thread (`Send_Task`).

### M55 (HOST, `lab/src/main.c`)

The M55 side is very simple. It registers `mbox_rx_callback()`, and when a message arrives, it checks the size, copies it with `memcpy`, and logs the `total_presses` value. Since this callback also only logs and makes no blocking calls, it is safe to run it directly in mbox callback context.

## Devicetree Configuration

This lab has two overlays.

**`lab/boards/sr100_rdk_sr100_m55.overlay` (M55 side)**
- `&ipc0 { shared-memory-size = <0x400>; };` — Specifies the size of the shared IPC memory region used by the M4 and M55 images. This value must match the corresponding setting in the M4 overlay exactly.
- `&i2c1 { status = "disabled"; };` — The M4 and M55 physically share the same I2C1 bus (the SCL/SDA pins). To avoid a bus-initialization conflict, i2c1 is disabled on the M55 side so that only the M4 actually drives `gpio_exp0` (the GPIO expander chip the button hangs off of).
- `&gpio_exp0 { status = "disabled"; };`, `&ov02c10 { status = "disabled"; };` — Disabling the parent bus (i2c1) alone is not enough. If a child node does not specify its own `status`, it keeps the default value `"okay"`, so the driver still treats the node as an "active instance" and tries to include it in the build. As a result, `DT_BUS()` fails to find the (disabled) i2c1 device handle, causing a build error. That's why the child nodes must also be disabled explicitly.

**`lab/remote/boards/sr100_rdk_sr100_m4.overlay` (M4 side)**
- `&ipc0 { shared-memory-size = <0x400>; };` — Same value as on the M55 side.
- `&buttons { polling-mode; };` — As explained in Core IPC Concept #2 above, `gpio_exp0` has no `int-gpios`, so interrupt-based `gpio-keys` cannot work; this switches to polling mode instead.

## How to Build

```bash
# 1) Build M4 (remote) first
west build -p always -b sr100_rdk/sr100/m4 ./zephyr_ipc_sr110/05_button_press_counter/lab/remote -d m4

# 2) Build M55 (host, embedding the M4 binary via M4_BUILD)
west build -p always -b sr100_rdk/sr100/m55 ./zephyr_ipc_sr110/05_button_press_counter/lab -d m55 \
    -DCONFIG_SR100_RELEASE_M4_RESET=y -DM4_BUILD="../m4"
```

> **Note**: `M4_BUILD` is a relative path from the M55 build directory (`m55/`). If `m4/` and `m55/` are sibling directories under the workspace root, `../m4` is correct.

## Running and Verifying the Result

After flashing the board, open the serial console and you should see logs like the following.

- **M4 console**: Every button press prints `button press #N sent to M55`, with N incrementing by one each time. Since it runs in polling mode, there is a delay of roughly `debounce-interval-ms` (30 ms by default) before it registers.
- **M55 console**: `total button presses so far: N` — the latest cumulative count sent by the M4, printed as-is.

Press the button several times and check that the N value on both consoles always matches.

## Summary

- Instead of forwarding every event as-is, **computing and sending the accumulated state (running total)** simplifies the receiver's logic and makes it more robust against message loss.
- The `polling-mode` property of `zephyr,gpio-keys` must always be applied to the **parent node** (`&buttons`), because of a hardware constraint on this board: `gpio_exp0` has no `int-gpios` wired up and therefore cannot support interrupt-based button input.
- Repetitive, resource-consuming work like low-level polling/debouncing is naturally assigned to the M4, which is closer to the hardware, while the M55 only consumes the result (the count) — this **division of labor** is a natural design choice in an AMP architecture.
- The principle from Lab 03 — no blocking calls in mbox callbacks/input callbacks, with the actual send handled in a dedicated worker thread — is preserved in this lab as well.

## Next Lab Preview

Lab 06 (Structured Command) covers sending a **structured command message** made up of multiple fields, rather than a single plain number, in the M55 → M4 direction. Where the flow so far has been about notifying state/events from one side to the other, from the next lab onward we'll cover the reverse flow — "issuing commands" — along with a somewhat more complex message structure.

---

Ran into a problem? → See [`05_button_press_counter_troubleshooting_en.md`](05_button_press_counter_troubleshooting_en.md)

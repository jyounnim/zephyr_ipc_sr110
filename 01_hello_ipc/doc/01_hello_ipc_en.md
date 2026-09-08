# Lab 01: Hello IPC — M55 → M4 Ping

This is the simplest possible inter-core communication (IPC) example on the SR110 (sr100_rdk, an asymmetric Cortex-M55 + Cortex-M4 dual-core board): the M55 sends a signal once per second, and the M4 receives it and toggles LED0. This lab is the starting point for the entire M4↔M55 IPC curriculum — it establishes the basic IPC skeleton (opening an mbox channel, registering a callback, handing off from an ISR to a worker thread) that every later lab reuses.

## Learning Objectives

By the end of this lab, you will be able to:

- Write code that exchanges a minimal signal between two cores using Zephyr's raw `mbox` driver API (`mbox_send_dt`, `mbox_register_callback_dt`, `mbox_set_enabled_dt`).
- Read and interpret the devicetree `mbox-consumer` properties (`mboxes`, `mbox-names`) to identify which hardware channel they point to.
- Explain why the mbox receive callback runs in **interrupt (ISR) context**, and independently implement the standard pattern for handling that safely (ISR → `k_msgq` → worker thread).
- Understand how the M4 and M55 split the IPC shared-memory region (`shared-memory-size`) between them, and keep the two cores' overlay settings consistent with each other.
- Reproduce this project's standard build order: build the M4 (remote) image first with `west build`, then include that result when building the M55 (host) image.

## Prerequisites / Connection to Other Labs

Lab 01 is the first lab in this curriculum. It assumes you're already comfortable with basic Zephyr RTOS thread/logging usage (`k_thread_create`, `LOG_INF`, etc.) and basic devicetree concepts (nodes, `status`, `&label` overlay syntax). No prior knowledge of mbox or IPC itself is required — introducing exactly that is the whole point of this lab.

This lab covers only **one-way communication, from M55 to M4**. Bidirectional communication, where the M4 replies to the M55, is covered formally in Lab 03 (although this lab's code already includes a small diagnostic M4→M55 reply for its own purposes — explained separately below).

## Core IPC Concepts

### Why do the M4 and M55 need to "communicate" at all?

The SR110 is an **asymmetric multi-processing (AMP)** SoC that packs a high-performance Cortex-M55 core and a low-power, real-time-oriented Cortex-M4 core into a single chip. The two cores boot and run completely separate Zephyr images independently of each other — M55 and M4 don't share a single OS instance; it's more accurate to think of them as **two entirely separate computers that happen to live on one chip**.

In this kind of architecture, the two cores can't cooperate (for example, having the M4 drive an LED based on a decision the M55 made) using techniques that only work within a single core, like direct function calls or shared global variables. Instead, they need a mechanism for exchanging information between **two physically separate execution units** — that mechanism is IPC (Inter-Processor Communication).

### What mbox is — a hardware perspective

`mbox` (mailbox) is a dedicated hardware block built into the SoC specifically for this kind of inter-processor communication. A mailbox analogy makes the core behavior easy to picture:

- When one core (say, the M55) has something to tell the other core, it writes a value into an mbox hardware register. This is "dropping a letter in the mailbox."
- That register write automatically triggers a **hardware interrupt (a doorbell)** on the other core (the M4). This is like "raising the flag next to the mailbox to signal that a letter has arrived."
- The M4, having received the interrupt, detects it in its interrupt service routine (ISR) and, if needed, reads the actual data (the payload) from a predetermined shared-memory region.

In other words, mbox itself is just "a signal line that raises an interrupt" — the actual data being exchanged (structs, etc.) lives in a separate **shared memory** region. mbox handles the "data is ready" notification (the doorbell); shared memory holds "the data itself." That's the division of labor. The `shared-memory-size` property that appears in this lab's devicetree defines the size of that shared-memory region.

### What Zephyr's mbox driver API abstracts away

Zephyr wraps this hardware mechanism in a standardized API (`zephyr/drivers/mbox.h`) so you don't have to deal with per-core register addresses directly. The three core functions used in this lab are:

- **`mbox_send_dt(spec, msg)`**: Sends a message (`msg`) over the channel defined in devicetree (`spec`). Internally, it writes to the mbox hardware registers to raise an interrupt on the peer core and writes the payload into the designated shared-memory location.
- **`mbox_register_callback_dt(spec, callback, user_data)`**: Registers a callback function to be invoked when a message arrives on this channel. As explained below, this callback runs in **ISR context**.
- **`mbox_set_enabled_dt(spec, 1)`**: Enables the channel so the registered callback actually fires on incoming interrupts. If you register a callback but forget to call this, nothing happens when a message arrives.

With just these three functions — and without needing to know the register layout of whatever mbox hardware a particular board has — you can write portable code that refers to channels purely by the names declared in devicetree.

### The devicetree `mbox-consumer` / `mboxes` properties

Before you can use an mbox channel in code, you first need to declare in devicetree which mbox channel(s) this application will use. This board's base devicetree already defines an `mbox-consumer` node in the following form:

```dts
mbox_consumer: mbox-consumer {
    mboxes = <&ipc0 1>, <&ipc0 0>;
    mbox-names = "tx", "rx";
};
```

The `mboxes` property is a list of `<mbox controller, channel number>` pairs, and `mbox-names` gives each of them a name (`tx`, `rx`). In code, you fetch a channel by that name, e.g. `MBOX_DT_SPEC_GET(DT_PATH(mbox_consumer), tx)`. Interestingly, the tx/rx channel numbers are mapped opposite to each other on the M4 and M55 — the channel the M55 sends on (tx) is the M4's receive channel (rx), the same way two people standing face-to-face across a mailbox each see it from the opposite side.

### The callback runs in ISR context — the most important concept in this lab

The callback function registered with `mbox_register_callback_dt()` runs in **interrupt service routine (ISR) context** the instant a message arrives. This carries fundamentally different constraints than running code in an ordinary thread.

- An ISR must finish very quickly. It blocks other interrupt handling and affects the responsiveness of the whole system.
- **You can never make a blocking call inside an ISR.** That includes obvious sleep functions like `k_msleep()`, but also any hardware access that might take a while to complete (e.g. an I2C transaction). Making a blocking call inside an ISR can, in the worst case, stall the entire kernel.

The M4-side code in this lab runs directly into this constraint. LED0, which the M4 must control, sits behind an I2C-connected GPIO expander (explained below), and an I2C transaction is inherently a blocking call. In other words, **you cannot toggle the LED directly from inside the mbox callback.**

The standard pattern for solving this is **ISR → `k_msgq` → worker thread**:

1. The mbox callback (running in ISR context) simply copies the incoming message into a kernel message queue (`k_msgq`) using `K_NO_WAIT`, then returns immediately. Enqueueing itself is a non-blocking operation, so it's safe to call from an ISR.
2. A separate worker thread waits on that queue with `k_msgq_get(..., K_FOREVER)`. When a message arrives, it wakes up and safely performs the actual time-consuming work (here, toggling the LED over I2C) in thread context.

This pattern — "do the absolute minimum in the interrupt handler, and hand off the real work to a thread" — is a core design principle that recurs, in various forms, throughout every later lab in this curriculum. Get comfortable with it here, and the rest of the labs will go much more smoothly.

## Architecture / Code Walkthrough

### The message struct (`lab/include/ipc_common.h`)

The message format that both the M55 and M4 need to agree on is defined once, in a header that both sides `#include`.

```c
/* M55 -> M4 ping message. */
struct ipc_ping_msg {
    uint32_t seq;
};
```

`seq` is a sequence number incremented by 1 on every send, letting you visually track "which ping just arrived" in the console log. Copying a **fixed-size struct that both sides define identically** back and forth as the mbox payload is the basic pattern used throughout this curriculum.

The same header also defines `struct ipc_ack_msg`. This lab's original scope was strictly one-way (M55 → M4), but the code includes a small self-diagnostic reply so the M55-side console can confirm that the M4 actually toggled the LED successfully (see the troubleshooting doc for the full background). The `led_ret` field carries the raw return value of `gpio_pin_toggle_dt()`: `0` means success, a negative value is an actual error code, and `-1000` is a sentinel meaning the LED device itself was never ready.

### M55 (HOST) side — `lab/src/main.c`

The M55 is the **sender** in this lab. The flow of `main()` is:

1. Fetch the tx/rx channel specs from the devicetree `mbox-consumer` node.

   ```c
   tx_channel = (struct mbox_dt_spec)MBOX_DT_SPEC_GET(DT_PATH(mbox_consumer), tx);
   rx_channel = (struct mbox_dt_spec)MBOX_DT_SPEC_GET(DT_PATH(mbox_consumer), rx);
   ```

2. Call `mbox_mtu_get_dt()` to check the channel's maximum transfer unit (MTU) and verify at boot time that the message we intend to send fits within it. This catches a mis-sized message as a clear, early error, rather than something that surfaces only when you actually try to send.
3. Register and enable a callback for receiving the diagnostic M4→M55 ack.
4. In a `while (1)` loop, increment `seq` and send a ping to the M4 with `mbox_send_dt()` every second (`PING_PERIOD_MS`).

The ack-receive callback (`mbox_ack_rx_callback`) does nothing more than log a line, which is light enough to run safely directly inside the ISR — a good example of a callback that's fine to leave in ISR context, consistent with the "no blocking work in an ISR" rule described above.

### M4 (CLIENT) side — `lab/remote/src/main.c`

The M4 is the side that **receives the signal and performs the actual action** (toggling LED0) in this lab. It implements the ISR → `k_msgq` → worker thread pattern described earlier, in full.

```c
K_MSGQ_DEFINE(rx_msgq, sizeof(struct ipc_ping_msg), 4, 4);
```

The mbox callback does nothing but enqueue and return immediately:

```c
static void mbox_rx_callback(...)
{
    struct ipc_ping_msg msg = {0};
    ...
    memcpy(&msg, data->data, sizeof(msg));

    /* ISR-safe: don't touch I2C (LED) here, just enqueue */
    if (k_msgq_put(&rx_msgq, &msg, K_NO_WAIT) != 0) {
        g_msgq_drop_count++;
        ...
    }
}
```

Notice that `k_msgq_put` is called with `K_NO_WAIT` — even if the queue is full, this never waits and returns failure immediately, which categorically rules out the ISR ever blocking. When the queue is full and a message has to be dropped, it's tallied in a drop counter (`g_msgq_drop_count`) and logged as a warning — but only on the first occurrence and every 10 thereafter, so the log doesn't get flooded.

The actual LED toggle happens in a dedicated `Toggle_Task` worker thread:

```c
static void toggle_task_entry(void *p1, void *p2, void *p3)
{
    ...
    while (1) {
        if (k_msgq_get(&rx_msgq, &msg, K_FOREVER) != 0) {
            continue;
        }
        if (led0_ready) {
            int ret = gpio_pin_toggle_dt(&led0);
            ...
        }
        ...
    }
}
```

This thread sleeps on `K_FOREVER` until a new message arrives in the queue, then wakes up and toggles LED0. `gpio_pin_toggle_dt()` internally performs an I2C transaction — a call that takes time and can block — but since this runs in ordinary thread context, it's perfectly safe to call here.

Also worth noting: in `main()`, if the LED0 device isn't ready, the code doesn't bail out of the function right there. Instead, it just records the state in an `led0_ready` flag and continues on to register the IPC callback and spawn the worker thread as usual. This lets you clearly distinguish between "something's wrong with the LED hardware" and "the IPC path itself is dead" as two different symptoms — a useful defensive habit when working with real hardware.

## Devicetree Configuration

The M55 and M4 images are built separately, each with its own devicetree, but a handful of values still need to **match exactly on both sides** for IPC to work. Both cores' overlay files (`lab/boards/sr100_rdk_sr100_m55.overlay`, `lab/remote/boards/sr100_rdk_sr100_m4.overlay`) therefore share this block:

```dts
&ipc0 {
    shared-memory-size = <0x400>;
};
```

`shared-memory-size` defines the size of the shared-memory region the two cores use to exchange data. If this value differs on either side, the two images end up assuming different memory layouts, and communication breaks. This lab actually only exchanges 4 bytes (a single `uint32_t seq`), so a much smaller value would suffice — but it's set to a uniform 1 KB up front in anticipation of larger messages in later labs.

The M55-side overlay also disables `&i2c1`, `&gpio_exp0`, and `&ov02c10` (`status = "disabled"`). This is due to a hardware characteristic of this board: the M4 and M55 physically share the same I2C1 bus. Since it's the M4 that actually drives LED0, the M55 side explicitly disables this bus so it doesn't claim it at all, preventing a bus-ownership conflict at boot. The full background and how this was discovered is documented in the troubleshooting doc.

## Build Instructions

Because this project's M55 (host) image references the M4 (remote) image's build output, **you must build the M4 first**. From the root of the west workspace (the directory where `zephyr/` is visible), run the following in order:

```bash
# 1) Build the M4 (remote) image
west build -p always -b sr100_rdk/sr100/m4 ./zephyr_ipc_sr110/01_hello_ipc/lab/remote -d m4

# 2) Build the M55 (host) image — including the M4 binary
west build -p always -b sr100_rdk/sr100/m55 ./zephyr_ipc_sr110/01_hello_ipc/lab -d m55 \
    -DCONFIG_SR100_RELEASE_M4_RESET=y -DM4_BUILD="../m4"
```

What each flag means:

- `-p always`: Fully clean (pristine) the build directory every time before building. This prevents stale cache or overlay settings from a previous build from producing unexpected results.
- `-b <board>/<qualifier>`: Specifies the target board and core. `sr100_rdk` is the board name; `/sr100/m4` and `/sr100/m55` are the qualifiers identifying the M4 and M55 cores respectively.
- `-d <directory>`: Specifies where to place the build output. M4 and M55 are built into separate directories (`m4`, `m55`).
- `-DCONFIG_SR100_RELEASE_M4_RESET=y`: A Kconfig option that, during M55 boot, releases the M4 core (which is held in reset) and starts it running. This reflects the standard role split in this curriculum, where the M55 acts as the "master" that boots the M4.
- `-DM4_BUILD="../m4"`: Specifies the location of the M4 build output that the M55 build references. This path is resolved **relative to the M55 build directory (the `m55/` directory created by `-d m55`), not the current working directory** — so with `m4/` and `m55/` as sibling directories in this layout, `../m4` is the correct value.

## Running It and Verifying the Result

The two cores each output their console over a separate UART, so open the M55 and M4 serial consoles separately, both at 230400 bps, 8N1. On correct operation you'll see the following logged once per second:

- **M55 console**: A ping-send log that increments every second, in the form `Ping sent, seq=1`, `seq=2`, ..., immediately followed by the diagnostic ack from the M4, in the form `[ACK] seq=N M4 alive, LED0 toggle OK`.
- **M4 console**: A log line once per second in the form `[Toggle_Task] ping seq=N -> LED0 toggle ret=0`, and at the same time, you can visually confirm the board's LED0 actually blinking once per second.

For flashing the image onto the board, follow the standard procedure for whichever board/toolchain setup you're using.

## Summary / Key Takeaways

In this lab, we implemented the simplest possible IPC flow: the M55 sends a signal to the M4 over mbox, and the M4 receives it and drives hardware (LED0) in response. The essential points are that **mbox is responsible only for the "data has arrived" signal (the interrupt), while the actual data is exchanged through a shared-memory region defined in devicetree**, and that **because the callback that receives that signal runs in ISR context, any blocking work must be handed off to a separate worker thread**. This ISR → `k_msgq` → worker thread pattern is, in one form or another, the single most important design principle running through every lab in this curriculum.

## Coming Up in the Next Lab

The next lab (Lab 02) adds a second direction of communication. Where this lab was a one-way signal from M55 to M4, the next lab covers a round-trip flow: a physical button connected to the M4 sends an event to the M55 over IPC, and the M55 receives that event and, in turn, controls a different LED on the M4. In other words, we move from "only the M55 sends signals" to "both sides exchange IPC messages triggered by events" — and along the way, we'll also deal with a hardware characteristic specific to this board: handling button input via polling rather than interrupt-driven input.

---

If you run into trouble, see `01_hello_ipc_troubleshooting_en.md`.
</content>

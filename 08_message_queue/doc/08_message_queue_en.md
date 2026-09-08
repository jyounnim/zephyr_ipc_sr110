# Lab 08: Multi-Type Message Queue — Queuing Several Kinds of Messages

In this lab, the M55 fires off four different LED pattern commands back-to-back every 6 seconds, with no delay between them, while the M4 pulls the messages off a `k_msgq` (depth 8) in arrival order and runs each one to completion before moving to the next — this is **continuous message-stream queuing**. Every lab up to this point has followed a "one signal in → one reaction out" pattern. This lab is the first to deliberately create a situation where **the producer emits several messages far faster than the consumer can process them**, so you can watch `k_msgq` absorb that speed mismatch firsthand.

## Learning Objectives

By the end of this lab, you will be able to:

- Explain why a `k_msgq`-based queuing pattern is needed once you move beyond single-shot request/response and have to handle a **stream of several kinds of messages arriving in succession**.
- Understand what the queue depth (capacity) parameter of `K_MSGQ_DEFINE` means, and how to size it based on the throughput gap between the producer (M55) and the consumer (M4).
- Know what `k_msgq_put(..., K_NO_WAIT)` returns when the queue is full, and how to handle that inside an ISR callback without ever blocking.
- Implement, on your own, a structure that processes messages of different kinds (tagged messages) in order, through a single queue and a single worker thread.
- Observe the queue's current backlog at runtime with `k_msgq_num_used_get()`, and log it to verify that queuing is actually happening.

## Continuity with Previous Labs

Labs 01 through 07 were, for the most part, built around "the M55 sends one signal, the M4 immediately performs the one corresponding action," with generous timing margins so that the next signal never arrived until the previous one had already been handled. Lab 07 (Echo Service) in particular established, through on-hardware verification, the principle that **no blocking call of any kind — not even `mbox_send_dt()` itself — may ever be made inside an `mbox` callback.**

Lab 08 carries that principle forward, while deliberately creating, for the first time, **a situation where the producer is already sending the next message before the consumer has finished handling the previous one.** The M55 sends four pattern commands consecutively with no delay in between, while the M4 needs `200ms × 2 × repeat` (1.2 to 1.8 seconds, depending on the pattern) to run each pattern to completion. In other words, the M55's send rate structurally outpaces the M4's processing rate — and absorbing that gap is exactly the central topic of this lab: the message queue.

## Core IPC Concepts

### Why "queuing" is needed instead of a "single-shot response"

In the earlier labs, a message arrived and was handled immediately (either right there or handed off to a worker thread), and by the time the next message showed up the previous one had already finished. In that kind of structure, a queue depth of 1 would have caused no real problem — because messages never had a chance to pile up.

In real systems, however, the producer's and the consumer's processing rates don't always line up. When one side "bursts" several events out in a short window — as in this lab — or when the consumer-side work (running an LED pattern, here) inherently takes time, there will always be a window where unprocessed messages accumulate. Without a queue, you're left with only two bad options:

1. Have the callback (ISR) **block** until the previous message finishes processing → blocking inside an ISR is a forbidden operation, so this was never really an option.
2. Simply **drop** the message that couldn't be processed → in a lab like this one, where each of the four distinct pattern commands carries its own meaning, dropping a message is a functional bug.

`k_msgq` gives us a third option: a **buffering** mechanism that lets us **temporarily hold, up to a fixed capacity, messages that can't be handled right now, and pull them out in order as the consumer becomes ready.** The ISR (the mbox callback) only has to perform a non-blocking enqueue, so it stays safe, and the consumer (the worker thread) can process messages at its own pace. What emerges is an **asynchronous pipeline** where the producer and consumer are free to run at different speeds.

### Why queue depth (capacity) sizing matters

In `K_MSGQ_DEFINE(pattern_msgq, sizeof(struct ipc_pattern_msg), 8, 4)`, the third argument, `8`, is the queue depth — the maximum number of messages it can hold at once (the fourth argument, `4`, is the alignment in bytes for each message). This value should never be picked arbitrarily; it needs to account for both **the maximum number of messages the producer can burst out at once** and **however many additional messages might arrive while the consumer is still working through that burst.**

In this lab, the M55 sends four patterns back-to-back once every cycle (6 seconds). Because the queue depth is generously set to 8, even if the M4 hasn't finished the previous cycle's messages by the time the next cycle's four arrive (worst case: up to 8 messages waiting at once), nothing is lost. Had the depth been set to something smaller than 4 — say, 3 — even a single cycle would have run into a situation where the fourth pattern couldn't be enqueued and was dropped.

When the queue is full, calling `k_msgq_put(&pattern_msgq, &msg, K_NO_WAIT)` does not block — it returns `-ENOMSG` immediately. This lab's callback doesn't check that return value (for the sake of simplicity), but in real production code it's important to count and log this failure so you can observe, during operation, whether the queue is actually overflowing — because ultimately, how generous the depth needs to be is a value that has to be validated against exactly this kind of observed data.

### Why this lab is already a "best-practice" design that reflects Lab 07's lesson

In Lab 07, an exception that had initially seemed reasonable — "it's fine to call a lightweight API directly from inside the mbox callback (ISR)" — was completely retired once on-hardware verification confirmed that `mbox_send_dt()` itself could block on this board. In other words, "make no IPC call whatsoever inside the ISR other than enqueuing" became the principle applied to every lab that followed.

Lab 08 reflects that principle from the design stage onward. Look at the M4's `mbox_rx_callback()` and you'll see it does exactly three things:

```c
memcpy(&msg, data->data, sizeof(msg));
k_msgq_put(&pattern_msgq, &msg, K_NO_WAIT);
```

This callback **never sends a reply back to the M55.** This lab is designed, from the outset, as a **one-way M55→M4 structure** with no M4→M55 response at all, so the question "should this callback call `mbox_send_dt()`?" never even arises. With no data to reply with, there's no reply code to write — and so this callback is never exposed to the risk Lab 07 uncovered, where `mbox_send_dt()` itself could block. The callback does nothing but enqueue; every task that actually takes time (running the LED pattern, including several blocking `k_msleep` calls) happens exclusively in a separate `Pattern_Task` worker thread. In that sense, Lab 08 isn't a lab that "happened to pick only the safe patterns" — it's a design where **the very subject of queuing naturally aligns with this principle.**

## Architecture / Code Walkthrough

### Message structure (`lab/include/ipc_common.h`)

```c
enum ipc_pattern_id {
    PATTERN_BLINK_LED0  = 1,
    PATTERN_BLINK_LED1  = 2,
    PATTERN_ALTERNATE   = 3,
    PATTERN_BOTH_FLASH  = 4,
};

struct ipc_pattern_msg {
    uint32_t pattern_id; /* enum ipc_pattern_id */
    uint32_t repeat;     /* number of times to repeat the pattern */
};
```

`pattern_id` acts as the tag that distinguishes the "several kinds of messages" in this lab. `repeat` controls how many times that pattern repeats, so that even messages of the same kind end up as work items of varying length each time (this makes the M4's per-message processing time vary run to run, so you get to observe a more varied range of queue backlog behavior).

### M55 (HOST) side — `lab/src/main.c`

In this lab, the M55 plays the role of **the producer that bursts messages out.**

```c
static void send_pattern(uint32_t pattern_id, uint32_t repeat)
{
    struct ipc_pattern_msg msg = {.pattern_id = pattern_id, .repeat = repeat};
    struct mbox_msg mbox_msg = {.data = &msg, .size = sizeof(msg)};

    mbox_send_dt(&tx_channel, &mbox_msg);
    LOG_INF("queued pattern_id=%u repeat=%u", pattern_id, repeat);
}
```

The loop in `main()` calls this function four times in a row before sleeping for 6 seconds.

```c
while (1) {
    send_pattern(PATTERN_BLINK_LED0, 3);
    send_pattern(PATTERN_BLINK_LED1, 3);
    send_pattern(PATTERN_ALTERNATE, 3);
    send_pattern(PATTERN_BOTH_FLASH, 2);

    k_msleep(6000);
}
```

There is deliberately no delay between the four `send_pattern()` calls. Since `mbox_send_dt()` triggers an interrupt on the M4 and returns immediately, this loop effectively pushes four messages toward the M4 almost instantaneously. Actually processing all four takes the M4 (3+3+3+2)×2×200ms = 4.4 seconds, so every single cycle reproduces the textbook queuing scenario from the M4's point of view: "everything arrives at once, and there's a long stretch of digesting it afterward."

### M4 (CLIENT) side — `lab/remote/src/main.c`

The M4 plays the role of **consuming messages off the queue, in order,** using exactly the ISR → `k_msgq` → worker thread pattern described above.

```c
K_MSGQ_DEFINE(pattern_msgq, sizeof(struct ipc_pattern_msg), 8, 4);

static void mbox_rx_callback(const struct device *dev, mbox_channel_id_t channel_id,
                              void *user_data, struct mbox_msg *data)
{
    struct ipc_pattern_msg msg;

    if (data->size < sizeof(msg)) {
        return;
    }
    memcpy(&msg, data->data, sizeof(msg));
    k_msgq_put(&pattern_msgq, &msg, K_NO_WAIT);
}
```

The callback does exactly three things — size validation, `memcpy`, and `k_msgq_put` — and returns immediately. As noted above, this callback never replies back to the M55, so absolutely everything that runs inside the ISR is a non-blocking operation.

The actual consumption happens in a separate worker thread called `Pattern_Task`.

```c
static void pattern_task_entry(void *p1, void *p2, void *p3)
{
    struct ipc_pattern_msg msg;

    while (1) {
        if (k_msgq_get(&pattern_msgq, &msg, K_FOREVER) != 0) {
            continue;
        }
        LOG_INF("running pattern_id=%u repeat=%u (queue depth now=%u)",
                msg.pattern_id, msg.repeat, k_msgq_num_used_get(&pattern_msgq));
        run_pattern(&msg);
    }
}
```

This thread waits on `K_FOREVER` for a new message to show up in the queue. Once it pulls one out, it logs, via `k_msgq_num_used_get()`, **exactly how many messages are still waiting in the queue at that instant**, and then calls `run_pattern()` to run that pattern to completion (including several `k_msleep(200)` calls). Because this function runs in thread context, it's safe to use blocking calls freely here. Depending on `pattern_id`, `run_pattern()` performs one of four actions — blink LED0 only, blink LED1 only, alternate the two LEDs, or flash both LEDs together — repeating it `repeat` times.

In `main()`, the mbox callback is registered and enabled, the `Pattern_Task` thread is spawned, and `main()` itself then goes to sleep with `k_sleep(K_FOREVER)`, leaving all of the real work to the worker thread and the ISR.

## Devicetree Configuration

As far as IPC itself is concerned, this lab's overlay files carry the same minimal configuration as the earlier labs.

```dts
/* shared between lab/boards/sr100_rdk_sr100_m55.overlay and lab/remote/boards/sr100_rdk_sr100_m4.overlay */
&ipc0 {
    shared-memory-size = <0x400>;
};
```

`shared-memory-size` is the size of the IPC shared-memory region, and it must be identical in both the M55 and M4 overlays. The message exchanged in this lab (`struct ipc_pattern_msg`, 8 bytes) is still small, but the value is kept at the same 1 KB used across the other labs for consistency.

The M55-side overlay also carries additional settings that disable `&i2c1`, `&gpio_exp0`, and `&ov02c10`.

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

The M4 and M55 physically share the same I2C1 bus (the GPIO expander that LED0/LED1 hang off of sits on this bus). Since only the M4 actually drives the LEDs in this lab, the M55 side explicitly disables this bus entirely, to avoid a boot-time bus-ownership conflict. How this issue was discovered is documented in the [Lab 01 troubleshooting document](../../01_hello_ipc/doc/01_hello_ipc_troubleshooting_en.md).

## How to Build

Build the M4 (remote) image first, then build the M55 (host) image so that it references the M4 build output. From the west workspace root, proceed in this order:

```bash
# 1) Build the M4 (remote) image
west build -p always -b sr100_rdk/sr100/m4 ./zephyr_ipc_sr110/08_message_queue/lab/remote -d m4

# 2) Build the M55 (host) image — includes the M4 binary
west build -p always -b sr100_rdk/sr100/m55 ./zephyr_ipc_sr110/08_message_queue/lab -d m55 \
    -DCONFIG_SR100_RELEASE_M4_RESET=y -DM4_BUILD="../m4"
```

`M4_BUILD` is resolved as a **relative path** with respect to the M55 build directory (`m55/`). Given that `m4/` and `m55/` are created as sibling directories under the workspace root in this structure, `../m4` is the correct value (see the [Lab 01 troubleshooting document](../../01_hello_ipc/doc/01_hello_ipc_troubleshooting_en.md) for more background).

## Running and Verifying the Result

Open a serial console for each core at 230400 bps, 8N1. On correct operation, you should see the following pattern repeat:

- **M55 console**: Every 6 seconds, four lines of `queued pattern_id=N repeat=N` print back-to-back, almost simultaneously — proof that the four messages are being pushed out with no delay between them.
- **M4 console**: `running pattern_id=N repeat=N (queue depth now=N)` prints one at a time, in the same order the messages arrived, and each time one of these lines prints, LED0/LED1 actually run that pattern in hardware until it completes (roughly 200ms × 2 × repeat). The first log line's `queue depth now` tends to print around 3 (the remaining three, after the one just dequeued), and you can watch that number shrink as the M4 works through them one by one — that changing number is direct evidence that queuing is actually taking place.

## Summary / Key Takeaways

In this lab, the M55 sends four different LED pattern messages back-to-back with no delay, and the M4 receives them through a `k_msgq` (depth 8), processing them one at a time, in order, to completion. The key points are:

- Since producer and consumer processing rates inevitably diverge, `k_msgq` provides the buffering that absorbs that difference.
- Queue depth should be sized around "the maximum number of messages the producer can burst out at once"; if it's undersized, `k_msgq_put(..., K_NO_WAIT)` fails (message loss).
- Because this lab's mbox callback is part of a one-way structure with no reply back to the M55, it was able to naturally satisfy the principle established in Lab 07 — "make no IPC call inside the callback other than enqueuing" — without needing any special-case handling.

## Coming Up Next

The next lab (Lab 09) flips who initiates communication and what kind of data flows. Up through this lab, the M55 has always been the one issuing commands to the M4; Lab 09 instead covers **M4 → M55 telemetry**, where the M4 samples its onboard accelerometer (MC3419) directly every 500ms and sends the resulting values (x, y, z, seq) to the M55. On top of that, the M55 side has no real blocking work to do beyond logging the values it receives, so it handles them directly in the ISR callback with no worker thread at all — a good contrast to this lab's reason for needing a queue in the first place (a slow consumer), and a useful case study in when a queue is *not* needed.

---

Ran into trouble? → see `08_message_queue_troubleshooting_en.md`


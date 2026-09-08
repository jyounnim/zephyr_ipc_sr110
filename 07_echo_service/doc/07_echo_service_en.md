# Lab 07: Echo Service — Verifying M55 ↔ M4 Round-Trip Reliability

M55 sends a test string to M4 every 2 seconds, and M4 echoes it straight back. M55 compares the returned string against what it originally sent and logs either `ECHO OK` or `ECHO MISMATCH`. There's no extra hardware here — no LEDs, no buttons — this lab verifies **nothing but the reliability of the mbox IPC round trip itself**.

## Learning Objectives

By the end of this lab, you will be able to:

- Explain why the echo pattern — "send back exactly what you received" — is a standard technique for validating the integrity of an IPC path (whether data is corrupted or lost in transit).
- Explain, from the perspective of RTOS ISR design principles, why even an API that looks lightweight on paper — like `mbox_send_dt()` — must never be called from inside a callback (ISR context).
- Apply, in your own code, this curriculum's final rule: the mbox rx callback must **without exception** "only enqueue and return immediately," while all real processing — including the actual send — is deferred to a worker thread.
- Read and reproduce the M4-side echo service structure, built from a `k_msgq` and a worker thread (`Echo_Task`).

## Connection to Previous Labs

Lab 01 established the M55→M4 one-way signaling pattern (ISR → `k_msgq` → worker thread), and the labs since then have exercised IPC in various directions and payloads. Lab 07 returns to the simplest, most fundamental test of all on top of that foundation: verifying that exactly the same content survives a round trip. And it is in this very lab that a rule we had followed only implicitly up to now — "no blocking in an ISR" — was confirmed on real hardware to apply **without exception to `mbox_send_dt()` itself**. This principle doesn't end with this one lab; it becomes the absolute rule for the entire curriculum, governing how every mbox callback is written in every lab from here on.

## Core IPC Concepts

### Why the echo service pattern is useful for testing IPC reliability

An echo service does something deliberately trivial: it returns received data unmodified. Simple as it sounds, it's a long-standing standard technique for validating networking and IPC stacks. The reasons are clear:

- Because the sender still holds the original, it can do a **direct, byte-for-byte comparison** against what comes back. This is the strongest form of round-trip validation available — something you can't get as cleanly from other message types (e.g. event notifications or counter values).
- Because there's almost no logic involved (just copy and send back), if a value ever comes back different, you can narrow the suspect down to the **IPC path itself** (payload-size handling, shared-memory offsets, timing, queuing, etc.) rather than an application-logic bug.
- Repeated transmission (every 2 seconds, in this lab) lets you observe patterns like "which message number does the problem start at" — which is especially valuable for catching state-accumulation bugs (queue slot exhaustion, resource leaks) that a one-shot test would never reveal.

This lab played exactly that role in this curriculum — the decisive design principle discussed below, that `mbox_send_dt()` must never be called directly from a callback, surfaced precisely because of this echo round-trip test.

### Why even `mbox_send_dt()` must not be called from inside a callback

In earlier labs, you learned that the mbox rx callback always runs in **ISR (Interrupt Service Routine) context**, and so you've followed the rule that obviously thread-blocking calls like `k_msleep()` must never be called there. That leaves one open question: "Surely `mbox_send_dt()` itself is fine, though — it's just a short function that writes a few registers, right?"

This lab is where that question got answered on real hardware, and the answer was: **no, it is not safe.**

Under general RTOS design principles, the only functions guaranteed safe to call from an ISR are ones explicitly documented as "ISR-safe" or "non-blocking." A function that looks lightweight on the surface can still block internally, for reasons such as:

- If called while a hardware queue or buffer is full, the driver may internally wait for a slot to free up.
- If the driver protects its internal state with a mutex or semaphore, the caller can end up waiting while another context holds that lock.
- If a new send is attempted before the previous message has been drained by the peer core, the implementation may simply wait until that drain completes.

This board's mbox backend turned out to be exactly this kind of case. Going by the API documentation alone, it was reasonable to assume "it's just a lightweight register write, so it must be non-blocking" — but in reality, depending on the driver implementation, this function could block internally. And once blocking occurs in ISR context, that core's entire interrupt handling and scheduler grind to a halt from that moment on. The Zephyr kernel operates on the assumption that ISRs are short and non-blocking; once that assumption is violated, there is no way to recover.

**This is what led to the final rule for this project:**

> Inside an mbox rx callback, use only calls with an explicit non-blocking guarantee — such as `k_msgq_put(..., K_NO_WAIT)` — and defer **everything else, including the `mbox_send_dt()` call, without exception, to a separate worker thread.**

Earlier documentation had left room for exceptions — "a call this lightweight can be allowed in the callback as a special case" — but after this lab's hardware verification, that exception was retired completely. Rather than relying on the assumption "this API looks lightweight, so it should be fine," the rule left standing is single and absolute: **the callback only enqueues.** The concrete sequence of events that actually froze the system on real hardware, along with how it was diagnosed, is covered in the [troubleshooting document](07_echo_service_troubleshooting_en.md).

## Architecture / Code Walkthrough

### M55 (HOST) — `lab/src/main.c`

M55 cycles through three test strings (`test_strings[]`), sending one every 2 seconds, and compares M4's reply against the original directly in the callback.

```c
static void mbox_rx_callback(const struct device *dev, mbox_channel_id_t channel_id,
                              void *user_data, struct mbox_msg *data)
{
    char buf[ECHO_MAX_LEN] = {0};
    size_t len = MIN(data->size, sizeof(buf) - 1);

    memcpy(buf, data->data, len);

    if (strcmp(buf, g_last_sent) == 0) {
        LOG_INF("ECHO OK: \"%s\"", buf);
    } else {
        LOG_WRN("ECHO MISMATCH: sent=\"%s\" got=\"%s\"", g_last_sent, buf);
    }
}
```

The M55-side callback does nothing but a string comparison and a log line — a task that finishes in a very short time, so it's safe to handle directly inside the ISR. This does not violate the "no blocking in an ISR" principle discussed above, because `strcmp` and `LOG_INF` are not blocking calls. (By contrast, the M4 side became a problem precisely because it tried to make a call — `mbox_send_dt()` — that actually can block, from inside its callback.)

The send loop is straightforward:

```c
while (1) {
    strncpy(g_last_sent, test_strings[idx % NUM_TEST_STRINGS], sizeof(g_last_sent) - 1);
    mbox_msg.data = g_last_sent;
    mbox_msg.size = strlen(g_last_sent) + 1; /* +1 to include the NUL terminator */

    LOG_INF("Sending: \"%s\"", g_last_sent);
    mbox_send_dt(&tx_channel, &mbox_msg);

    idx++;
    k_msleep(2000);
}
```

This call runs in the ordinary thread context of `main()`, so it's fine even if `mbox_send_dt()` blocks internally. Blocking is only dangerous when it happens in **ISR context**.

### M4 (CLIENT) — `lab/remote/src/main.c`

The M4 side is the heart of this lab. The mbox callback does nothing but copy the received bytes into a `struct echo_item` and enqueue it.

```c
K_MSGQ_DEFINE(echo_msgq, sizeof(struct echo_item), 4, 4);

static void mbox_rx_callback(const struct device *dev, mbox_channel_id_t channel_id,
                              void *user_data, struct mbox_msg *data)
{
    struct echo_item item;

    item.len = MIN(data->size, sizeof(item.data));
    memcpy(item.data, data->data, item.len);

    /* ISR-safe: no blocking calls here, just enqueue for the worker thread */
    k_msgq_put(&echo_msgq, &item, K_NO_WAIT);
}
```

The key detail is that `k_msgq_put` is called with `K_NO_WAIT` — even if the queue is full, it never waits and returns failure immediately, so the callback cannot block under any circumstance. There is no `mbox_send_dt()` call anywhere in this callback.

The actual reply happens exclusively in a dedicated worker thread, `Echo_Task`.

```c
static void echo_task_entry(void *p1, void *p2, void *p3)
{
    struct echo_item item;
    struct mbox_msg reply;

    LOG_INF("[Echo_Task] Started on M4");

    while (1) {
        if (k_msgq_get(&echo_msgq, &item, K_FOREVER) != 0) {
            continue;
        }

        LOG_INF("rx %u bytes, echoing back", (unsigned int)item.len);

        /* mbox_send_dt() may block on this board's mbox backend -- that's
         * fine here, we're in thread context, not an ISR. */
        reply.data = item.data;
        reply.size = item.len;
        mbox_send_dt(&tx_channel, &reply);
    }
}
```

`Echo_Task` sleeps on `K_FOREVER` until a new item lands in the queue, then wakes up and calls `mbox_send_dt()`. Even if this call blocks internally, `Echo_Task` is an ordinary thread, so the Zephyr scheduler can keep running other threads (including the logging thread) normally — this is the decisive difference between an ISR and a thread.

After registering/enabling the callback and creating `Echo_Task`, `main()` does nothing further and simply sleeps.

```c
k_thread_create(&echo_task_data, echo_task_stack,
                K_THREAD_STACK_SIZEOF(echo_task_stack),
                echo_task_entry, NULL, NULL, NULL,
                ECHO_TASK_PRIORITY, 0, K_NO_WAIT);
k_thread_name_set(&echo_task_data, "Echo_Task");
```

This clean separation of roles — **callback (ISR) does only enqueueing, worker thread (`Echo_Task`) does all real processing** — is the final rule for this curriculum, confirmed by this lab. Keep in mind once more: even a function whose name alone sounds lightweight, like `mbox_send_dt()`, must be called only from the worker thread, without exception.

## Devicetree Configuration

Both the M55 and M4 overlays keep `ipc0`'s `shared-memory-size` identical.

```dts
&ipc0 {
    shared-memory-size = <0x400>;
};
```

This value is the size of the IPC memory region shared by the two cores; if the two sides don't match, the memory layout goes out of sync and communication breaks. The strings exchanged in this lab are at most `ECHO_MAX_LEN` (64 bytes), so 1 KB leaves plenty of headroom.

The M55-side overlay (`lab/boards/sr100_rdk_sr100_m55.overlay`) also disables `&i2c1`, `&gpio_exp0`, and `&ov02c10`. This lab uses neither LEDs nor a camera, but because the M4 and M55 physically share the same I2C1 bus on this board, this disable is kept common across every lab on the M55 side, to prevent bus-ownership conflicts (for the full background, see the [Lab 01 troubleshooting document](../../01_hello_ipc/doc/01_hello_ipc_troubleshooting_en.md)).

## How to Build

From the west workspace root, build M4 (remote) first, then build M55 (host), including the M4 build output.

```bash
# 1) Build the M4 (remote) image
west build -p always -b sr100_rdk/sr100/m4 ./zephyr_ipc_sr110/07_echo_service/lab/remote -d m4

# 2) Build the M55 (host) image — include the M4 binary via M4_BUILD
west build -p always -b sr100_rdk/sr100/m55 ./zephyr_ipc_sr110/07_echo_service/lab -d m55 \
    -DCONFIG_SR100_RELEASE_M4_RESET=y -DM4_BUILD="../m4"
```

`M4_BUILD` is a relative path from the M55 build directory (`m55/`). If `m4/` and `m55/` are sibling directories under the workspace root, `../m4` is correct (for the full explanation, see the [Lab 01 troubleshooting document](../../01_hello_ipc/doc/01_hello_ipc_troubleshooting_en.md)).

## Running and Checking the Results

Connect to both cores' serial consoles and check for the following logs.

- **M55 console**: every 2 seconds, `Sending: "..."` is printed, followed by `ECHO OK: "..."` on a successful round trip. If the content doesn't match, `ECHO MISMATCH: sent="..." got="..."` is printed instead.
- **M4 console**: `[Echo_Task] Started on M4` is printed once at boot, and thereafter `rx N bytes, echoing back` is printed each time a message is received.

Under normal operation, both the M55 and M4 logs keep advancing without interruption, once every 2 seconds.

## Summary / Key Takeaways

- The echo pattern (returning received data unmodified) is a useful technique for isolating and verifying the reliability of the IPC path itself, with application logic excluded from the picture.
- **`mbox_send_dt()` must never be called directly from inside a callback (ISR), without exception.** Never assume "it looks lightweight, so it must be non-blocking" based on its name or appearance alone — depending on the driver implementation, it can block internally.
- The mbox rx callback's only responsibility is to enqueue data with `k_msgq_put(..., K_NO_WAIT)` and return immediately; everything else, including the `mbox_send_dt()` call, must happen in a separate worker thread (`Echo_Task`).
- This rule is not confined to this one lab — it is this curriculum's final principle, to be followed whenever an mbox callback is written in any lab from here on.

## Next Lab Preview

The next lab (Lab 08) goes beyond a single message round trip and digs more deeply into `k_msgq`-based message queue handling under conditions where multiple messages can pile up in succession. Building on the principle established in this lab — "the callback only enqueues, processing happens in the worker thread" — it explores extensions such as what happens when the queue fills up, and patterns for consuming multiple messages in order.

---

Curious about the actual debugging process that led to this design? → See `07_echo_service_troubleshooting_en.md` (it covers a real case where the system hung on actual hardware)

# Lab 10: Threshold Event Notification

Instead of sending every single sample, this lab teaches a conditional/event-driven IPC pattern in which M4 notifies M55 only **when the accelerometer reading crosses a threshold**.

## Learning Objectives

- Understand the difference and trade-offs between polling/periodic transmission and event-driven transmission.
- Understand why decisions must be made on **deviation from a baseline** rather than raw sensor values, and design a boot-time calibration step yourself.
- Learn how to use debounce to suppress duplicate events that occur close together in time.
- Learn the pattern of handling command reception and periodic work (polling) together in **a single thread** using a `k_msgq` timeout.

## Connecting to the Previous Lab: From "Send Every Sample" to "Send Conditionally"

In Lab 09 (`09_accel_telemetry`), M4 read the accelerometer at a fixed period and sent the value to M55 **every single time**. M55 simply logged whatever it received. This approach is simple to implement and guarantees M55 always has the latest value, but it also means mbox traffic and logging keep happening even during stretches where the value barely changes.

Lab 10 uses the same sensor (MC3419) but changes only the logic on the M4 side: it still polls a sample every 100 ms, but it sends an event to M55 **only when that sample represents a meaningful change (a threshold crossing)**. The M55-side code actually becomes simpler — it sets the threshold once at boot, and from then on it only needs to react when an event arrives.

## Core IPC Concepts

### 1. Event-Driven vs. Polling/Periodic Transmission

| | Polling/Periodic (Lab 09) | Event-Driven (Lab 10) |
|---|---|---|
| When it sends | Unconditionally, every fixed period | Only when a condition is met |
| Bandwidth | Constant, regardless of how the value changes | Near zero at rest, consumed only on events |
| Receiver burden | Must parse/process every time (even if the value hasn't changed) | Wakes only for meaningful situations |
| Knowing current state | Always has the latest value | Must assume "quiet since the last event = below threshold" |
| Missed events | Recovers on the next cycle regardless | If an event itself is lost (e.g. an mbox send failure), the receiver has no way to know without a separate re-check |

In general, polling/periodic transmission is preferable when values are needed frequently/continuously (e.g. real-time control loops) or when a missed sample would be critical. Conversely, for alert/notification-style data that is normally quiet and only needs a reaction in specific situations (temperature anomalies, shock detection, button events, etc.), event-driven transmission saves resources significantly. That said, event-driven designs don't guarantee "the system is still alive since it last went quiet," so in practice a separate heartbeat is often layered on top to distinguish "no events = everything is normal" from "no events = the link is down" (a topic covered later in this curriculum).

### 2. Comparing Deviation After Baseline Calibration — Why Relative, Not Absolute, Values Matter

An accelerometer always carries roughly 1g of gravitational acceleration on some axis, even at rest. If you judge purely by "does the raw magnitude exceed the threshold," then even a board sitting perfectly still already outputs a value near or above the threshold from gravity alone, producing continuous false-positive events at rest. In other words, what we actually want to detect isn't "how large a raw value is the sensor reporting right now," but "**how much has it changed relative to its normal (baseline) state**."

This lab uses the following design pattern:

1. For about 1 second right after boot (`CALIB_SAMPLES` samples), repeatedly sample the sensor to compute the X/Y/Z averages, and store these as the **baseline**.
2. From then on, every sample is compared to the threshold not as a raw value but as `current value - baseline` (deviation).

This absorbs and cancels out the gravity offset and any fixed tilt present at boot time into the baseline, leaving only the deviation that reflects actual "movement" occurring afterward. This baseline is **a fixed snapshot taken once at boot**, not a continuously updating adaptive filter — so if the board is moved during the calibration window (roughly the first second after boot), that motion itself gets mixed into the baseline and skews every judgment made afterward.

This "compare deviation from baseline" pattern isn't specific to accelerometers — it's a general design technique applicable to any sensor application where what matters is "how far it has drifted from a normal state" rather than the absolute magnitude itself (temperature drift, pressure, ambient light, gyro bias, and so on).

### 3. Debounce — Why It's Needed

When a value hovers right around the threshold, it can cross back and forth multiple times within a short window of sampling. Sending an event for every single crossing wastes bandwidth and floods the receiver's log with entries that carry no additional meaning. This lab records the timestamp of the last event sent and, within `DEBOUNCE_MS` (500 ms) of that last event, suppresses any further events no matter how much the threshold is exceeded. In other words, it prevents a single "movement/shock" from being reported as several duplicate events, and instead groups it into the unit a human would naturally expect (one event = one occurrence).

### 4. Handling Command Processing and Periodic Sampling in One Thread via a `k_msgq` Timeout

Rather than running two separate threads, the M4-side `Threshold_Task` in this lab uses a single worker thread that handles both jobs at once.

```c
while (1) {
    /* Process a command if one is waiting; otherwise time out after
     * POLL_PERIOD_MS and treat that as the sampling tick */
    if (k_msgq_get(&cmd_msgq, &cmd, K_MSEC(POLL_PERIOD_MS)) == 0) {
        if (cmd.cmd == IPC10_CMD_SET_THRESHOLD) {
            threshold_milli_g = cmd.threshold;
            LOG_INF("[Threshold_Task] threshold updated to %d", threshold_milli_g);
        }
        continue;
    }

    /* k_msgq_get() timed out -> no command arrived within POLL_PERIOD_MS,
     * so this iteration is a normal sampling tick */
    ...
}
```

Giving `k_msgq_get()` a `K_MSEC(POLL_PERIOD_MS)` (100 ms) timeout means it returns immediately if a command is already waiting in the queue, and returns via timeout after 100 ms if nothing is there. The core of this pattern is reusing that timeout itself as the signal "poll the sensor once every 100 ms." The result:

- With no separate timer or second thread, a command that arrives is handled immediately (with near-zero added latency) rather than waiting for the next polling tick.
- With no command pending, the sensor is read at exactly the polling period.
- Because there's only one thread, shared state like `threshold_milli_g` needs no separate lock — it is always read and written from within the same thread.

## Architecture Summary

- **M55 (host, `lab/src/main.c`)**: At boot, sends the `IPC10_CMD_SET_THRESHOLD` command to M4 once to set the threshold (default 2000 milli-g), and thereafter only logs when an event arrives in `rx_cb()`.
- **M4 (client, `lab/remote/src/main.c`)**: A single `Threshold_Task` handles both command processing and 100 ms polling. It calls `calibrate_baseline()` right after boot to capture the baseline, and from then on sends an event via `mbox_send_dt()` only when a given sample's deviation (magnitude) from baseline exceeds the threshold and the debounce window has elapsed.

`calibrate_baseline()`:

```c
static void calibrate_baseline(int32_t *base_x, int32_t *base_y, int32_t *base_z)
{
    int64_t sum_x = 0, sum_y = 0, sum_z = 0;
    int good_samples = 0;
    int32_t x, y, z;

    for (int i = 0; i < CALIB_SAMPLES; i++) {
        if (read_accel_milli(&x, &y, &z) == 0) {
            sum_x += x;
            sum_y += y;
            sum_z += z;
            good_samples++;
        }
        k_msleep(POLL_PERIOD_MS);
    }
    ...
    *base_x = (int32_t)(sum_x / good_samples);
    *base_y = (int32_t)(sum_y / good_samples);
    *base_z = (int32_t)(sum_z / good_samples);
}
```

Samples are collected for `CALIB_SAMPLES` (10) × `POLL_PERIOD_MS` (100 ms) ≈ 1 second and averaged, and that result becomes the baseline used as the reference for every judgment thereafter.

The event-decision logic:

```c
int32_t dx = x - base_x;
int32_t dy = y - base_y;
int32_t dz = z - base_z;
int32_t mag = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy) + (dz < 0 ? -dz : dz);

int64_t now = k_uptime_get();
if (mag > threshold_milli_g && (now - state.last_event_ms) > DEBOUNCE_MS) {
    struct ipc10_event_msg evt = {.x = x, .y = y, .z = z, .seq = ++seq};
    struct mbox_msg mbox_msg = {.data = &evt, .size = sizeof(evt)};

    mbox_send_dt(&tx_channel, &mbox_msg);
    state.last_event_ms = now;
    ...
}
```

`mag` approximates the deviation from baseline as a sum of absolute values (an L1-norm-like form). An mbox event is actually sent only when this value exceeds the threshold *and* `DEBOUNCE_MS` has elapsed since the last event.

### mbox Callbacks Must Never Block

Both `rx_cb()` implementations, on M55 and M4, return immediately — M4 only enqueues the command with `k_msgq_put(..., K_NO_WAIT)`, and M55 only logs the event's contents. All the actual work (accelerometer polling/calibration, threshold updates, and the `mbox_send_dt()` call itself) happens entirely in the `Threshold_Task` worker thread, so this lab continues to follow the "mbox callbacks must never block" principle established in Lab 03/Lab 07.

## Devicetree Configuration

The `ipc0` shared-memory-size must match exactly between the M4 and M55 overlays (`0x400`).

The M4 overlay (`lab/remote/boards/sr100_rdk_sr100_m4.overlay`) enables the accelerometer node:

```
&mc3479 {
	status = "okay";
};
```

The devicetree node label for the MC3419 accelerometer is **`mc3479`**, not `accel0`, and its default status is `"disabled"`, so it must be explicitly enabled like this.

The M55 overlay (`lab/boards/sr100_rdk_sr100_m55.overlay`) disables `&i2c1` (and its child nodes) to avoid conflicting over the I2C1 bus that M4 and M55 physically share — I2C1 is used only by M4.

## How to Build

```bash
# 1) M4 (remote)
west build -p always -b sr100_rdk/sr100/m4 ./zephyr_ipc_sr110/10_threshold_event/lab/remote -d m4

# 2) M55 (host, includes the M4 binary as M4_BUILD)
west build -p always -b sr100_rdk/sr100/m55 ./zephyr_ipc_sr110/10_threshold_event/lab -d m55 \
    -DCONFIG_SR100_RELEASE_M4_RESET=y -DM4_BUILD="../m4"
```

`M4_BUILD` is a relative path from the M55 build directory (`m55/`). If `m4/` and `m55/` are sibling directories at the workspace root, `../m4` is correct.

## Running and Checking the Results

After flashing both images and rebooting the board:

- **M4 console**: Right after boot, `[Threshold_Task] calibrating baseline (~1s, keep the board still)...` is printed, followed about a second later by `[Threshold_Task] baseline x=.. y=.. z=.. (from 10 samples)`. Once a command arrives from M55, `threshold updated to 2000` is printed, and whenever motion exceeding the threshold is detected, `event sent seq=N mag=N (dev x=.. y=.. z=..)` is printed.
- **M55 console**: `threshold set to 2000 milli-g` is printed once at boot. After that, `THRESHOLD EVENT seq=N x=.. y=.. z=..` appears only when you shake the board hard enough to cross the threshold; it's expected behavior for no further logs to appear at all once calibration finishes if the board is left still.

> **Note**: Do not move the board during calibration (roughly the first second after boot). If you do, that motion gets mixed into the baseline itself, skewing every judgment made afterward.

## Summary

- Unlike Lab 09's "send every sample," Lab 10 sends events only "when a condition is met," substantially cutting both IPC traffic and receiver-side processing load.
- Sensor readings must be judged by their **deviation from a boot-time-calibrated baseline**, not by raw values, to avoid false positives at rest. This pattern generalizes well beyond accelerometers to many other sensor applications.
- Debounce prevents a single physical event from being reported as multiple duplicate events.
- Merging command processing and periodic polling into a single thread via one `k_msgq` timeout gets you both responsiveness and lock-free simplicity, without a separate timer or a second thread.

## Coming Up Next

Lab 11 covers display (OLED) output, teaching how to show events/data directly on screen instead of only checking them via console logs as done so far.

---

If you run into trouble → see [`10_threshold_event_troubleshooting_en.md`](./10_threshold_event_troubleshooting_en.md)

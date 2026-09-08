# Lab 14: Heartbeat Watchdog

## Learning Objectives

- Design the simplest possible IPC channel: a single periodic beacon (heartbeat) message flowing in one direction only.
- Learn the pattern of calling `k_msgq_get()` with a **finite timeout** so M55 can tell both "did a message arrive" and "how long have I been waiting" at once.
- Confirm that a tagged union isn't always the right call — when only one message kind ever exists, skip the `type` tag entirely.
- Learn how to simulate a heartbeat outage without actually hanging M4 (its console stays alive throughout).
- Apply the "M4-local vs. M55-relayed" design principle established in Lab 13, this time to **every** command.

## Connection to Earlier Labs

Lab 14 adds no new hardware at all. It uses exactly two things, all reused as-is:

1. M4's console UART (the `uart_poll_in()` polling command-intake path proven in Lab 13).
2. The mbox channel (`ipc0`), used by every lab since Lab 01.
3. M55's ST7789V3 TFT (the same raw-SPI driver as Lab 11/12/13).

This lab does not touch the AHT20/BMP280 sensors from Lab 12/13 at all — the subject here is the heartbeat/watchdog IPC pattern itself, not sensor telemetry.

## Why Not Trigger a Real Hang

A watchdog's whole purpose is detecting that M4 has died. But actually crashing M4 or dropping it into an infinite loop for a classroom demo creates real problems:

- It isn't a repeatable demo — the board has to be reflashed every time.
- Actually halting M4 also kills the console log we'd want to watch during recovery.

So this lab instead provides a command (`1 <1..60>`) that pauses **only the heartbeat-sending thread** for N seconds. The rest of M4 (console, command processing) keeps running normally. This is a deliberate simplification: from M55's point of view, there is no way to distinguish "M4 genuinely stopped" from "M4 only paused its heartbeat send" — all that's visible is silence on the channel. That simplification is enough to faithfully demonstrate M55's timeout-detection logic.

## Core Concepts

### 1. Skip the tagged union when there's only one message kind

Lab 06 and Lab 13 needed to multiplex several message kinds (sensor data, commands, responses, and so on) on one mbox channel, so they used a tagged union (`ipc13_msg`) with a `type` field. Lab 14 only ever sends one kind of message (heartbeat), one direction (M4 → M55). A tag would serve no purpose here, so the payload struct is sent directly.

```c
/* ipc_common.h -- no tag, exactly one message kind exists */
struct ipc14_heartbeat_payload {
	uint32_t seq;
};
```

**Lesson**: a tagged union earns its keep only when multiple message shapes genuinely share a channel. Lab 13's three message kinds needed the tag; this lab's single kind would only gain complexity from one.

### 2. Implementing "monitoring" with a finite-timeout `k_msgq_get()`

M55's `Watchdog_Task` does this:

```c
int ret = k_msgq_get(&hb_msgq, &hb, K_MSEC(WATCHDOG_CHECK_PERIOD_MS));
if (ret == 0) {
	/* heartbeat arrived -- normal */
} else if (ever_seen && state == WD_OK &&
           (k_uptime_get() - last_seen_ms) >= WATCHDOG_TIMEOUT_MS) {
	/* no heartbeat for >= WATCHDOG_TIMEOUT_MS -- timeout */
}
```

Blocking with `K_FOREVER` gives no way to observe "no message arrived" at all. Polling with `K_NO_WAIT` burns CPU continuously. A finite timeout like `K_MSEC(N)` sits in between — "wait up to N ms, and if nothing showed up, wake up and compute the elapsed time yourself." `WATCHDOG_CHECK_PERIOD_MS` (200ms) is the check cadence, while `WATCHDOG_TIMEOUT_MS` (1200ms, 4× the 300ms heartbeat period) is the threshold that actually declares a timeout. These are kept separate so a short check cadence doesn't itself cause premature timeout declarations — a timeout is only declared once several heartbeats in a row have genuinely been missed.

### 3. State machine: WD_WAITING → WD_OK → WD_TIMEOUT

```c
enum wd_state { WD_WAITING, WD_OK, WD_TIMEOUT };
```

- `WD_WAITING`: right after boot, no heartbeat has ever been received yet (gray "WAIT").
- `WD_OK`: heartbeats are arriving normally (green "OK").
- `WD_TIMEOUT`: silence has lasted at least `WATCHDOG_TIMEOUT_MS` (red "TIMEOUT"). The moment a heartbeat arrives again, it snaps straight back to `WD_OK`.

Every time a timeout fires, a `trips` counter increments and is shown on the TFT, so the screen also tracks how many timeouts have occurred so far.

### 4. Every command is M4-local — contrast with Lab 13

In Lab 13, commands 1/2/3 were relayed to M55 (a full mbox round trip), and only command 4 (log on/off) was handled locally on M4. Lab 14 is the opposite — **every** command is M4-local. Pausing, resuming, or querying the heartbeat sender is purely M4's own internal state (`pause_until_ms`), so there is no reason for any of it to touch mbox.

| Command | Meaning | Relayed to M55? |
|---|---|---|
| `1 <1..60>` | Pause heartbeat sending for N seconds (simulated hang) | No (M4-local) |
| `2` | Immediately clear the pause and resume heartbeats | No (M4-local) |
| `3` | Query current status (RUNNING / PAUSED, time remaining) | No (M4-local) |

### 5. A 64-bit timestamp is protected with `k_mutex`, not `atomic_t`

Lab 13's `display_mode` flag was a plain small integer, so `atomic_t` was sufficient. This lab's `pause_until_ms` is a 64-bit (`int64_t`) value, the return type of `k_uptime_get()`. On this platform, `atomic_t` cannot atomically hold a 64-bit value, so a `k_mutex pause_lock` protects access between `Uart_Cmd_Task` (writer) and `Heartbeat_Task` (reader) instead.

### 6. No actual M4 reset action is implemented

A complete watchdog pattern would normally continue past "detect timeout" into "trigger an M4 reset." But at the point this curriculum was being planned, no API for restarting M4 at runtime (e.g. `CONFIG_SR100_RELEASE_M4_RESET`) had been confirmed available on real hardware. Rather than guess at an unverified API, this lab implements **detection + display only** — showing the timeout state and cumulative trip count on the TFT is enough to fully demonstrate the observable core of the watchdog pattern.

## Architecture Diagram

```
M4 (Cortex-M4)                              M55 (Cortex-M55)
───────────────                              ─────────────────
Heartbeat_Task                               Watchdog_Task
  |                                            |
  | pause_until_ms == 0?                       | k_msgq_get(K_MSEC(200))
  |   yes -> mbox_send_dt(seq++) ---- mbox ---> rx_cb() -> k_msgq_put()
  |   no  -> skip this cycle                   |
  | k_msleep(300ms)                            | on arrival: WD_OK, update TFT SEQ
  |                                            | on none: elapsed >= 1200ms?
Uart_Cmd_Task                                  |            -> WD_TIMEOUT, trips++
  | uart_poll_in() polling, 10ms                | TFT shows status (WAIT/OK/TIMEOUT)
  | "1 N"/"2"/"3" -> handle_command()          |
  |   (all M4-local, mbox never used)           |
```

## Pin Connections (full wiring actually used in this lab — identical to Lab 11/12/13, no new wiring)

### ST7789V3 TFT (M55, SPI0)

| Panel Pin | Connects To | Notes |
|---|---|---|
| VCC | 3.3V | |
| GND | GND | |
| SCL/CLK | SPI0 CLK | |
| SDA/MOSI | SPI0 MOSI | |
| CS | SPI0 CS | |
| RES/RST | SoC GPIO17 (J24 pin 3) | **Level shifter required** — SoC GPIO is 3.3V, which can mismatch some panels' RST/DC input thresholds, so it is routed through a level shifter |
| DC | SoC GPIO18 (J24 pin 4) | Same level-shifter routing as above |
| BLK (backlight) | 3.3V or a dedicated GPIO | Tying it directly to 3.3V also works |

**Caution**: turning on SPI0 requires turning off M55's only UART (UART1, shared with GPIO23/24). The J14 USB-C connector and the J25 header are not two separate UARTs -- they are two different physical paths (an onboard USB-serial bridge for J14, an external converter header for J25) exposing the exact same UART1 signal. So disabling UART1 silences BOTH J14 and J25 -- from this lab onward, M55 gives up its serial console entirely, and **the TFT screen is the only way to check M55's state** (the design established in Lab 11, see the Notes section). This lab's `lab/prj.conf` already sets `CONFIG_UART_CONSOLE=n` and `CONFIG_CONSOLE=n`, and the `LOG_INF`/`printk` calls left in the M55 source are kept only for reading later if the console is ever re-enabled for debugging -- they don't actually go anywhere right now.

### Operator Command Input (M4 console UART, no new wiring)

| Target | Connection |
|---|---|
| M4 console (USB-to-TTL converter) | J24 header pin 13 = M4 TX, pin 14 = M4 RX, shared GND (converter TX↔board RX, converter RX↔board TX, crossed) — 230400bps, 8N1 |

This is exactly the same connection verified in Lab 13. This lab doesn't use any sensors (I2C0) at all, so M4's overlay is even simpler than Lab 12/13's.

## Devicetree Configuration

M4's overlay (`remote/boards/sr100_rdk_sr100_m4.overlay`) keeps only the `ipc0` shared-memory-size setting and drops all I2C0-related configuration — this lab doesn't use any sensors. M55's overlay (`boards/sr100_rdk_sr100_m55.overlay`) keeps the exact same TFT/SPI0 setup as Lab 13.

## prj.conf

- `lab/remote/prj.conf` (M4): `CONFIG_I2C=y` is gone (no sensor use). UART command intake works the same as Lab 13, needing no extra Kconfig beyond the default `CONFIG_SERIAL`.
- `lab/prj.conf` (M55): same as Lab 13 — SPI/GPIO enabled, M55's console UART disabled.

## How to Build

Run these from the west workspace root (the directory that has `zephyr/` in it). **On SR110, the M4 image must be built first, and the M55 build then pulls that M4 binary in via `M4_BUILD` to package it together** — this ordering was established starting with Lab 12; skipping it means the final flash image ends up with no M4 firmware in it at all (see the M4_BUILD troubleshooting note referenced in earlier labs).

```bash
# 1) Build the M4 (remote) image first
west build -p always -b sr100_rdk/sr100/m4 ./zephyr_ipc_sr110/14_heartbeat_watchdog/lab/remote -d m4

# 2) Build the M55 (host) image -- pulls in the M4 binary built above via M4_BUILD
west build -p always -b sr100_rdk/sr100/m55 ./zephyr_ipc_sr110/14_heartbeat_watchdog/lab -d m55 \
    -DCONFIG_SR100_RELEASE_M4_RESET=y -DM4_BUILD="../m4"
```

`M4_BUILD` is a path relative to the M55 build directory (`-d m55`). If `m4/` and `m55/` are sibling directories under the workspace root, `../m4` is correct.

## Running It and Checking the Results

1. Open M4's console (J24, 230400bps). M55 has no serial console from this lab onward (see the Caution note above), so there is no separate M55 console to open -- check M55's state on the TFT screen instead.
2. After flashing and resetting both cores, M4 begins sending a heartbeat every 300ms, and M55's TFT initially shows a gray "WAIT", switching to green "OK" the moment the first heartbeat arrives.
3. Type `3` into the M4 console to check status — it should print `[M4] status: RUNNING (heartbeat active)`.
4. Type `1 5` into the M4 console to pause heartbeats for 5 seconds — it prints `[M4] heartbeat PAUSED for 5s (simulated hang) -- watch M55's TFT`. Watching M55's TFT, roughly 1200ms (`WATCHDOG_TIMEOUT_MS`) after the last heartbeat, the status turns red "TIMEOUT" and the TRIPS counter increments by 1.
5. After the 5 seconds elapse, M4 automatically resumes, and the moment the next heartbeat reaches M55, the TFT flips back to green "OK".
6. Typing `2` before a `1 5` pause finishes resumes immediately — it prints `[M4] heartbeat RESUMED`.
7. Typing an out-of-range value like `1 70` prints `[M4] pause seconds must be 1..60` and does nothing.

## Summary

Without any new hardware, Lab 14 verified on real hardware the most basic pattern for monitoring an IPC channel — actively detecting "silence" using `k_msgq_get()` with a finite timeout. Contrasted with Lab 13, it also confirmed that a tagged union isn't always needed, and that not every operator command has to be relayed to the other core. An actual M4 reset trigger has no verified API and was left out of this lab's scope.

**Real-hardware verification complete.** Commands `1 <1..60>` (pause), `2` (resume), and `3` (status), together with M55's TFT transitioning WAIT → OK → TIMEOUT and the TRIPS counter incrementing, were all confirmed working correctly on real hardware.

## Notes

- Lab 13 (`13_uart_bridge`) — the source of the `uart_poll_in()` polling command-intake pattern this lab reuses, and the troubleshooting doc explaining why interrupt-driven RX doesn't work on this UART.
- Lab 11 (the TFT display lab) — the source of the ST7789V3 raw-SPI driver this lab reuses as-is, and the detailed explanation of the level shifter and SPI0 pin-sharing constraints.

# Lab 16: Capstone Gateway — Integrating Every Pattern

## Learning Objectives

- Run every pattern established across this curriculum — raw I2C/SPI drivers implemented directly (Lab 11/12), multi-sensor fallback with two independent fault indicators (Lab 12), active recovery (Lab 12), the per-command decision of M4-local vs. relayed-to-M55 (Lab 13), a finite-timeout watchdog (Lab 14), and Always-On/Wake low-power sync that sends only on state change (Lab 15) — together on one tagged-union IPC protocol.
- Confirm how four M4 threads, each with a different period and a different character (environment sensor watch, motion detection, heartbeat, command intake), safely share one mbox tx channel (mutex serialization).
- On the M55 side, implement a design where **a single queue, a single thread** handles several message kinds while also separately watching for the "absence" of just one of them (the heartbeat) — and see firsthand what tradeoff arises when Lab 14's finite-timeout watchdog and Lab 15's send-only-on-state-change event handling have to coexist in one thread.

## Connection to Earlier Labs

Lab 16 adds no new hardware at all. Everything this lab uses was already wired and verified in an earlier lab.

1. The AHT20+BMP280 combo module (M4, I2C0) — identical wiring and identical raw I2C driver to Lab 12/13/15.
2. The onboard MC3419 accelerometer (M4, I2C1) — identical wiring and identical Zephyr sensor subsystem usage (including the ODR + FULL_SCALE setup) to Lab 09/10.
3. Polling command intake on M4's console UART (M4, `uart_poll_in()`) — the exact pattern proven in Lab 13/14/15.
4. The ST7789V3 TFT (M55, SPI0) — the same raw-SPI driver as Lab 11/12/13/14/15.
5. The mbox channel (`ipc0`) — reused from every lab.

## Why This Lab Is Fundamentally Different From Earlier Labs

Most earlier labs dealt with "one sensor (or one axis)" and "one kind of event." Lab 16 is the opposite: four independently-running M4 threads each send mbox messages at their own pace, and M55 has to receive them all through **one channel, one tagged union** and react differently depending on the kind. This lab's central question isn't "how do I read a new sensor" but **"what has to be coordinated so several already-proven patterns can combine into one system without colliding?"**

## Core Concepts

### 1. A tagged union multiplexes four message kinds

```c
enum ipc16_msg_type {
	IPC16_MSG_ENV = 0,
	IPC16_MSG_MOTION,
	IPC16_MSG_HEARTBEAT,
	IPC16_MSG_STATUS,
};

struct ipc16_msg {
	uint8_t type;
	union {
		struct ipc16_env_payload env;
		struct ipc16_motion_payload motion;
		struct ipc16_heartbeat_payload heartbeat;
		struct ipc16_status_payload status;
	} payload;
};
```

Lab 14/15 each carried exactly one message kind, so no tag was needed ("a tag only earns its keep when several message kinds genuinely share a channel"). This lab, like Lab 06/13, genuinely multiplexes several kinds on one channel, so the tag comes back.

### 2. Four M4 threads share one tx channel — serialized with `tx_lock`

`Env_Task` (200ms period), `Motion_Task` (100ms period), `Heartbeat_Task` (1000ms period), and `Uart_Cmd_Task` (on command `4`) can all call `mbox_send_dt()`. So that any two of them sending at once never interleave, every send is serialized through the `k_mutex tx_lock` established in Lab 13.

```c
static void send_msg(struct ipc16_msg *msg)
{
	struct mbox_msg mbox_msg = {.data = msg, .size = sizeof(*msg)};

	k_mutex_lock(&tx_lock, K_FOREVER);
	mbox_send_dt(&tx_channel, &mbox_msg);
	k_mutex_unlock(&tx_lock);
}
```

### 3. ENV/MOTION send only on state change (Lab 15), HEARTBEAT sends unconditionally on a period (Lab 14) — two send philosophies in one system

`Env_Task` keeps reading the AHT20+BMP280 every 200ms, but an mbox send only happens on an **actual state change** — the temperature crosses the threshold, or the error mask changes (Lab 15's principle). `Motion_Task` applies the same principle to the accelerometer (Lab 10's baseline calibration + Lab 15's send-only-on-state-change).

`Heartbeat_Task` is the opposite — it sends **unconditionally** every second, whether anything happened or not (Lab 14's principle). This difference is not an accident: ENV/MOTION exist to report "something happened," so there is nothing to send when nothing changed; HEARTBEAT exists to prove the fact that "M4 is still alive," which is only provable if it keeps arriving on a strict, predictable schedule — there would be no way to detect its absence otherwise.

### 4. M55: one queue, one thread handles four message kinds AND the watchdog timer

```c
int ret = k_msgq_get(&msgq, &msg, K_MSEC(WATCHDOG_CHECK_PERIOD_MS));

if (ret == 0) {
	switch (msg.type) {
	case IPC16_MSG_ENV: /* update TFT */ break;
	case IPC16_MSG_MOTION: /* update TFT */ break;
	case IPC16_MSG_HEARTBEAT: /* transition watchdog to OK */ break;
	case IPC16_MSG_STATUS: /* update TFT */ break;
	}
	continue;
}
/* timed out -- check whether the heartbeat has been silent >= WATCHDOG_TIMEOUT_MS */
```

Lab 15 blocked indefinitely with `K_FOREVER` ("absence of a message carries no meaning there"). This lab can't do that — the absence of HEARTBEAT is exactly what needs watching. So `k_msgq_get()` uses the same **finite timeout** as Lab 14 (`WATCHDOG_CHECK_PERIOD_MS` = 500ms). When a message arrives (`ret == 0`), it's dispatched by tag; when the call times out (`ret != 0`), the code checks whether the time since the last heartbeat now exceeds `WATCHDOG_TIMEOUT_MS` (4000ms, 4x the heartbeat period). An ENV/MOTION/STATUS message arriving does NOT reset the watchdog timer — only a HEARTBEAT does, since the two are measuring different things and shouldn't be conflated.

### 5. Naming the tradeoff instead of hiding it: Lab 15's "fully idle wait" vs. this lab's "periodic timer check"

Lab 15's M55, when nothing was happening, blocked on `K_FOREVER` and let Zephyr's idle thread put the CPU into a WFI low-power wait. This lab must wake up every 500ms to check the clock for the watchdog, so it cannot stay in as deep an idle state as Lab 15. This is not a bug — it is a **deliberate design tradeoff**: wanting safety supervision (a watchdog) means accepting some amount of periodic wakeup as its cost. This lab names that tradeoff instead of hiding it.

### 6. Three of four commands are M4-local, one is relayed — Lab 13's principle, once more

| Command | Meaning | Relayed to M55? |
|---|---|---|
| `1 <celsius>` | Set the ENV (temperature) threshold | No (M4-local, same as Lab 15) |
| `2 <milli-g>` | Set the MOTION (acceleration) threshold | No (M4-local, same as Lab 10) |
| `3` | Print everything M4 currently knows (ENV/MOTION/heartbeat/uptime) to M4's own console | No (M4-local, same as Lab 14) |
| `4` | Push a full snapshot to M55 right now as `IPC16_MSG_STATUS` | **Yes — the one command in this lab that is relayed** |

Setting or locally querying thresholds (1/2/3) is purely M4's own business and has nothing to do with M55. `4` is the exception because "force the latest snapshot onto M55's screen right now" is a request that only means something once it reaches M55. Lab 13's principle — that whether a command needs to reach the other core is a per-command decision, not an all-or-nothing rule — is confirmed once again here, this time landing on exactly 1 of 4 commands.

## Architecture Diagram

```
M4 (Cortex-M4)                                        M55 (Cortex-M55)
────────────────                                       ─────────────────
Env_Task (200ms)                                       Sync_Task
  | Read AHT20+BMP280 (I2C0)                              | k_msgq_get(K_MSEC(500))
  | state change? -> mbox_send_dt(ENV) ------- mbox --->  | dispatch by tag:
  |                                                       |   ENV     -> update TFT
Motion_Task (100ms)                                       |   MOTION  -> update TFT
  | Read MC3419 (I2C1)                                    |   HEARTBEAT -> transition to WD_OK,
  | state change? -> mbox_send_dt(MOTION) ----- mbox --->  |                update last-seen time
  |                                                       |   STATUS  -> update TFT
Heartbeat_Task (1000ms, unconditional)                     | timed out (500ms)?
  | mbox_send_dt(HEARTBEAT) --------------------- mbox --->|   last heartbeat >= 4000ms ago?
  |                                                       |     -> WD_TIMEOUT, TRIPS++
Uart_Cmd_Task (polling)                                    |
  | "1 N"/"2 N"/"3" -> handled M4-locally                  rx_cb() -> k_msgq_put(msgq)
  | "4" -> mbox_send_dt(STATUS) ----------------- mbox --->  (ISR-safe, all handling in Sync_Task)
  |   (all serialized through tx_lock)
```

## Pin Connections (full wiring actually used in this lab — identical to Lab 09~15, no new wiring)

### AHT20 + BMP280 (M4, I2C0)

| Module Pin | Connects To | Notes |
|---|---|---|
| VCC | 3.3V | |
| GND | GND | |
| SDA | SoC GPIO3 (I2C0 SDA) | `i2c0_ms_sda`, internal pull-up enabled |
| SCL | SoC GPIO4 (I2C0 SCL) | `i2c0_ms_scl`, internal pull-up enabled |

### Onboard Accelerometer MC3419 (M4, I2C1)

This part is already mounted on the onboard `&i2c1` bus that M4/M55 physically share, so no wiring is needed. M4's overlay enables it with `&mc3479 { status = "okay"; };`, while M55's overlay disables `&i2c1`/`&gpio_exp0`/`&ov02c10` entirely to avoid a bus-ownership conflict (the rule carried since Lab 01).

### ST7789V3 TFT (M55, SPI0)

| Panel Pin | Connects To | Notes |
|---|---|---|
| VCC | 3.3V | |
| GND | GND | |
| SCL/CLK | SPI0 CLK | |
| SDA/MOSI | SPI0 MOSI | |
| CS | SPI0 CS | |
| RES/RST | SoC GPIO17 (J24 pin 3) | **Level shifter required** |
| DC | SoC GPIO18 (J24 pin 4) | Same level-shifter routing as above |
| BLK (backlight) | 3.3V or a dedicated GPIO | Tying it directly to 3.3V also works |

**Caution**: turning on SPI0 requires turning off M55's only UART (UART1, shared with GPIO23/24). The J14 USB-C connector and the J25 header are not two separate UARTs -- they are two different physical paths (an onboard USB-serial bridge for J14, an external converter header for J25) exposing the exact same UART1 signal. So disabling UART1 silences BOTH J14 and J25 -- from this lab onward, M55 gives up its serial console entirely, and **the TFT screen is the only way to check M55's state** (the design established in Lab 11, see the Notes section). This lab's `lab/prj.conf` already sets `CONFIG_UART_CONSOLE=n` and `CONFIG_CONSOLE=n`, and the `LOG_INF`/`printk` calls left in the M55 source are kept only for reading later if the console is ever re-enabled for debugging -- they don't actually go anywhere right now.

### Operator Command Input (M4 console UART, no new wiring)

| Target | Connection |
|---|---|
| M4 console (USB-to-TTL converter) | J24 header pin 13 = M4 TX, pin 14 = M4 RX, shared GND (converter TX↔board RX, converter RX↔board TX, crossed) — 230400bps, 8N1 |

## Devicetree Configuration

M4's overlay carries the same I2C0 setup as Lab 12/13/15 (for AHT20+BMP280) alongside the same `&mc3479 { status = "okay"; };` as Lab 09/10 (for the accelerometer). M55's overlay keeps the same TFT/SPI0 setup and the `&i2c1`/`&gpio_exp0`/`&ov02c10` disables as Lab 11 through 15.

## prj.conf

- `lab/remote/prj.conf` (M4): `CONFIG_I2C=y` (for raw-I2C AHT20/BMP280) plus `CONFIG_SENSOR=y`/`CONFIG_MC3419=y` (for the accelerometer's sensor subsystem) plus UART polling command intake (no extra Kconfig needed).
- `lab/prj.conf` (M55): SPI/GPIO enabled, M55's console UART disabled — same as Lab 11 through 15.

## How to Build

Run these from the west workspace root (the directory that has `zephyr/` in it). **On SR110, the M4 image must be built first, and the M55 build then pulls that M4 binary in via `M4_BUILD` to package it together** — this ordering has been established since Lab 12; skipping it means the final flash image ends up with no M4 firmware in it at all.

```bash
# 1) Build the M4 (remote) image first
west build -p always -b sr100_rdk/sr100/m4 ./zephyr_ipc_sr110/16_capstone_gateway/lab/remote -d m4

# 2) Build the M55 (host) image -- pulls in the M4 binary built above via M4_BUILD
west build -p always -b sr100_rdk/sr100/m55 ./zephyr_ipc_sr110/16_capstone_gateway/lab -d m55 \
    -DCONFIG_SR100_RELEASE_M4_RESET=y -DM4_BUILD="../m4"
```

`M4_BUILD` is a path relative to the M55 build directory (`-d m55`). If `m4/` and `m55/` are sibling directories under the workspace root, `../m4` is correct.

## Running It and Checking the Results

1. Open M4's console (J24, 230400bps). M55 has no serial console from this lab onward (see the Caution note above), so there is no separate M55 console to open -- check M55's state on the TFT screen instead.
2. After flashing and resetting both cores, M4 runs a ~1-second accelerometer baseline calibration right after boot (a "keep the board still" log). M55's TFT shows the title along with gray ENV/MOTION badges ("ENV --"/"MOTION --") and a gray "WAIT" watchdog badge.
3. Within about a second, M4 sends its first ENV event (the initial state) and its first heartbeat, and the TFT fills in — the ENV badge turns green "ENV OK" (or red "ENV HIGH"), and the watchdog badge turns green "OK".
4. Type `3` into M4's console to check M4's complete current state locally.
5. Warm the sensor by cupping it in your hand or holding a warm object nearby, raising the temperature above the threshold (default 28C) -- the TFT's ENV badge immediately turns red "ENV HIGH".
6. Shake or tilt the board -- the TFT's MOTION badge turns red "MOTION!", then returns to green "MOTION OK" a moment after the motion stops.
7. Type `4` into M4's console -- M4's console prints `STATUS pushed to M55`, and the STATUS line at the bottom of the TFT updates to something like `STATUS:#1 up=NNs`. Typing `4` again bumps the counter to `#2`.
8. Change the thresholds from M4's console with `1 <celsius>`/`2 <milli-g>`, and confirm the ENV/MOTION badges react against the new thresholds.
9. **Checking the watchdog**: this lab has no "deliberately hang" command like Lab 14's. Instead, force M4 to stop (if you have a way to do that without resetting it) or simply unplug M4's power to make the heartbeat go silent -- after 4 seconds (`WATCHDOG_TIMEOUT_MS`), confirm the TFT's watchdog badge turns red "TIMEOUT" and the TRIPS count increments by 1.

## Summary

Lab 16 combined nearly every pattern established across this curriculum -- raw I2C/SPI drivers, multi-sensor fallback with two independent fault indicators, active recovery, send-only-on-state-change, a finite-timeout watchdog, and per-command M4-local/relayed decisions -- into one system with no new hardware. In particular, it confirmed in code that Lab 15's "fully idle wait" and Lab 14's "periodic safety supervision" aren't mutually exclusive, but making them coexist does require accepting some amount of periodic wakeup as the price.

**Real-hardware verification complete.** With only M4's console open, the ENV/MOTION badges switching color on threshold crossings, commands `1`/`2`/`3` (M4-local) and `4` (a STATUS push relayed to M55) all working correctly, and the fact that M55 has no serial console at all once SPI0 (the TFT) is active (see the troubleshooting doc for the full story) -- all of it was confirmed working on real hardware.

## Notes

- Lab 15 (`15_low_power_sync`) — the source of this lab's ENV/MOTION send-only-on-state-change principle and the `K_FOREVER` vs. finite-timeout contrast.
- Lab 14 (`14_heartbeat_watchdog`) — the source of this lab's heartbeat/watchdog state machine (`WD_WAITING`/`WD_OK`/`WD_TIMEOUT`).
- Lab 13 (`13_uart_bridge`) — the source of the `uart_poll_in()` polling command-intake pattern this lab reuses, and the per-command M4-local/relayed decision principle.
- Lab 12 (`12_aht20_bmp280_logging`) — the source of this lab's AHT20+BMP280 raw I2C driver, multi-sensor fallback, dual fault indicators, and active recovery logic.
- Lab 09/10 (`09_accel_telemetry`, `10_threshold_event`) — the source of this lab's accelerometer baseline calibration and threshold-event logic.
- Lab 11 (`11_tft_sensor_display`) — the source of the ST7789V3 raw-SPI driver this lab reuses as-is, and the detailed explanation of the level shifter and SPI0 pin-sharing constraints.

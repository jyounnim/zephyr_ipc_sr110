# Lab 15: Low Power Sync

## Learning Objectives

- Reproduce this SoC's actual design intent — **M4 stays Always-On watching a lightweight sensor, while M55 normally idles and wakes only for a meaningful event** — directly as an IPC pattern.
- Contrast "send every time the sensor is read" (the periodic-send pattern used in Lab 11/12/13) with "send only when the state actually changes" (this lab), in code.
- Understand how blocking `k_msgq_get()` with `K_FOREVER` differs from Lab 14's finite timeout, and why `K_FOREVER` is the right choice here.
- Use Zephyr's idle thread — which automatically parks the CPU in a low-power wait (WFI) whenever no thread is runnable — to verify on real hardware that "M55 genuinely rests while waiting for an event," with no extra power-management configuration at all.

## Connection to Earlier Labs

Lab 15 adds no new hardware at all.

1. The AHT20 sensor (M4, I2C0) — identical wiring to Lab 12/13, using only AHT20 this time (BMP280 is not used).
2. The mbox channel (`ipc0`) — reused from every lab.
3. Polling command intake on M4's console UART (the `uart_poll_in()` pattern proven in Lab 13/14) — reused as-is.
4. M55's ST7789V3 TFT (the same raw-SPI driver as Lab 11/12/13/14) — reused as-is.

BMP280 is deliberately left unused in this lab because the subject here is the "always-on watch → wake only on event" pattern itself, not multi-sensor handling. A single temperature reading is enough — and clearer — for demonstrating this pattern.

## Why This Lab Is Fundamentally Different From Earlier Labs

Lab 11 (accelerometer streaming) and Lab 12/13 (environmental sensor logging) all shared the same structure: M4 sends to M55 every time it reads the sensor (or on a fixed period). In that design, an mbox message goes out on every cycle regardless of whether the value actually changed, and M55 has to keep receiving and processing every one of them.

This SoC was originally designed the other way around. M4 stays powered at low cost and keeps watching a lightweight sensor, while M55 (which owns power-hungry blocks like the NPU) should normally rest when it has nothing to do, waking up only when M4 decides "this is worth reporting." Lab 15 is the first lab to actually implement this structure: M4 reads the AHT20 every 500ms (cheap, since this stays entirely local to M4), but it notifies M55 over mbox **only at the moment the temperature crosses the threshold, in either direction**.

## Core Concepts

### 1. "Send every sample" vs. "send only on state change"

```c
/* Excerpt from the main loop in remote/src/main.c */
enum ipc15_temp_state new_state = (temp >= thresh) ? IPC15_TEMP_ABOVE : IPC15_TEMP_BELOW;

if (!have_state || new_state != current_state) {
	/* mbox_send_dt() is called ONLY here -- only when the state actually changed */
	mbox_send_dt(&tx_channel, &mbox_msg);
	current_state = new_state;
	have_state = true;
}
```

AHT20 is still read every 500ms, but an mbox send only happens on a state change (plus once at startup). If the temperature stays near the threshold for a long time, there may be no mbox traffic at all for minutes on end — that is expected, correct behavior.

### 2. `K_FOREVER` vs. Lab 14's finite timeout — why they differ

Lab 14's `Watchdog_Task` used a **finite timeout**, `k_msgq_get(&hb_msgq, &hb, K_MSEC(200))`, because that lab needed to actively detect the absence of a message (silence = a fault condition).

This lab's `Sync_Task` is the opposite case:

```c
if (k_msgq_get(&evt_msgq, &evt, K_FOREVER) != 0) {
	continue;
}
```

This lab assigns no meaning at all to "no message arrived" — if the temperature never crosses the threshold, staying silent indefinitely is entirely normal, and there is no reason to watch a clock for it. Blocking indefinitely with `K_FOREVER` is therefore the more correct design. Placing these two labs side by side makes the dividing line clear: use a timeout when you must detect an *absence*, and skip it when only *presence* matters.

### 3. Blocking on `K_FOREVER` is itself a low-power wait

Zephyr's idle thread automatically parks the CPU in a low-power wait state such as WFI (Wait For Interrupt) whenever no thread is currently runnable. As long as `Sync_Task` is blocked on `K_FOREVER` and M55 has nothing else to do, M55 is already "resting until an event arrives" without any dedicated power-management Kconfig (such as `CONFIG_PM`). The mbox interrupt waking M55 up is the only way out of that wait state.

**Caveat**: this is the Zephyr kernel scheduler's default idle behavior, and is distinct from explicitly entering one of the SoC's deeper low-power modes (Suspend-to-idle, System Off, and so on). While preparing this curriculum, exactly which SoC low-power states M55 supports, and what else `CONFIG_PM` would require to enable them, was not verified on real hardware. Rather than guess at an unverified Kconfig, this lab implements only what already holds true **with no extra configuration**: the idle thread's automatic WFI wait.

### 4. Every command is M4-local, too — the same principle as Lab 14

| Command | Meaning | Relayed to M55? |
|---|---|---|
| `1 <celsius>` | Set the threshold temperature as an integer Celsius value (default 28C) | No (M4-local) |
| `2` | Query the current temperature/threshold/state/event count sent so far | No (M4-local) |

Setting or querying the threshold is purely M4's own internal state, so it has nothing to do with M55. `threshold_milli_c` is a 32-bit integer, so `atomic_t` is sufficient, the same as Lab 13's `display_mode` (in contrast to Lab 14's 64-bit `pause_until_ms`, which needed a `k_mutex`).

### 5. Still no tagged union

Just like Lab 14, this lab only ever sends one kind of message (`ipc15_event_payload`) in one direction (M4 → M55) over mbox. The reasoning for skipping the tag is the same as in the Lab 14 doc.

## Architecture Diagram

```
M4 (Cortex-M4, Always-On)                    M55 (Cortex-M55, normally idle/WFI)
──────────────────────────                    ──────────────────────────────
Main loop (every 500ms)                       Sync_Task
  | Read AHT20 (I2C0, cheap)                    | k_msgq_get(K_FOREVER)  <- idle thread
  | new_state = (temp >= thresh)                 |                          enters WFI here
  |   ? ABOVE : BELOW                            |
  | new_state != current_state?                  |
  |   yes -> mbox_send_dt(evt) --- mbox --->    rx_cb() -> k_msgq_put()
  |          (wakes M55)                         |   -> Sync_Task wakes up
  |   no  -> no mbox send, next sample            |   -> TFT update (TEMP/EVENTS/color)
  | k_msleep(500ms)                              |   -> back to K_FOREVER wait

Uart_Cmd_Task                                  (M55 has no command console -- every
  | "1 N"/"2" -> handle_command()                command is M4-local, so nothing
  |   (all M4-local, mbox never used)             is ever sent to M55 for commands)
```

## Pin Connections (full wiring actually used in this lab — identical to Lab 11/12/13/14, no new wiring)

### AHT20 (M4, I2C0)

| Module Pin | Connects To | Notes |
|---|---|---|
| VCC | 3.3V | |
| GND | GND | |
| SDA | SoC GPIO3 (I2C0 SDA) | `i2c0_ms_sda`, internal pull-up enabled |
| SCL | SoC GPIO4 (I2C0 SCL) | `i2c0_ms_scl`, internal pull-up enabled |

This lab's code never touches the BMP280 side of the same combo module, but the wiring itself can remain identical to Lab 12/13 (there's no need to physically remove BMP280).

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

**Note**: enabling SPI0 requires disabling M55's UART1 console (it shares GPIO23/24). This lab's `lab/prj.conf` already sets `CONFIG_UART_CONSOLE=n` and `CONFIG_CONSOLE=n`. To view M55's console, you must use the **direct USB-C connection (J14, 115200bps)**.

### Operator Command Input (M4 console UART, no new wiring)

| Target | Connection |
|---|---|
| M4 console (USB-to-TTL converter) | J24 header pin 13 = M4 TX, pin 14 = M4 RX, shared GND (converter TX↔board RX, converter RX↔board TX, crossed) — 230400bps, 8N1 |

## Devicetree Configuration

M4's overlay keeps the exact same I2C0 setup as Lab 12/13 (the code only uses AHT20, but the bus configuration itself is unchanged). M55's overlay keeps the exact same TFT/SPI0 setup as Lab 11/12/13/14.

## prj.conf

- `lab/remote/prj.conf` (M4): `CONFIG_I2C=y` plus UART polling command intake (no extra Kconfig needed) — same as Lab 13/14.
- `lab/prj.conf` (M55): SPI/GPIO enabled, M55's console UART disabled — same as Lab 11 through 14. `CONFIG_PM` is deliberately left off (see Core Concept 3 above).

## How to Build

Run these from the west workspace root (the directory that has `zephyr/` in it). **On SR110, the M4 image must be built first, and the M55 build then pulls that M4 binary in via `M4_BUILD` to package it together** — this ordering has been established since Lab 12; skipping it means the final flash image ends up with no M4 firmware in it at all.

```bash
# 1) Build the M4 (remote) image first
west build -p always -b sr100_rdk/sr100/m4 ./zephyr_ipc_sr110/15_low_power_sync/lab/remote -d m4

# 2) Build the M55 (host) image -- pulls in the M4 binary built above via M4_BUILD
west build -p always -b sr100_rdk/sr100/m55 ./zephyr_ipc_sr110/15_low_power_sync/lab -d m55 \
    -DCONFIG_SR100_RELEASE_M4_RESET=y -DM4_BUILD="../m4"
```

`M4_BUILD` is a path relative to the M55 build directory (`-d m55`). If `m4/` and `m55/` are sibling directories under the workspace root, `../m4` is correct.

## Running It and Checking the Results

1. Open both the M4 console (J24, 230400bps) and the M55 console (J14 USB-C, 115200bps).
2. After flashing and resetting both cores, M55's TFT initially shows a gray "WAIT".
3. Right after boot, M4 sends the initial state (ABOVE or BELOW) to M55 based on its first AHT20 reading — the TFT immediately switches to green (BELOW) or red (ABOVE), and TEMP/EVENTS are populated.
4. Type `2` into the M4 console to check the current state — it prints something like `[M4] status: temp=26.500C threshold=28.000C state=BELOW events_sent=1`.
5. Warm the sensor by cupping it in your hand or holding a warm object nearby, raising the temperature above the threshold (default 28C). The instant it crosses the threshold, M55's TFT immediately switches to red "ABOVE" and the EVENTS count increments by 1. The M4 console logs `state change: BELOW -> ABOVE @ ...`.
6. Let it cool back down — the instant it drops below the threshold, the TFT switches back to green "BELOW" and EVENTS increments by 1 again.
7. Try changing the threshold with something like `1 30` — it prints `[M4] threshold set to 30C (M4-local -- not sent to M55)`, and the new threshold applies starting from the next state transition.
8. While the temperature stays stable near the threshold, confirm that the M4 console keeps printing new readings every 500ms (depending on log level), but that no mbox send or M55 screen update happens at all — this is the whole point of this lab.

## Summary

Lab 15 is the first lab to implement this SoC's actual design intent: "M4 Always-On watch → M55 wakes only for a meaningful event." In contrast with earlier labs' "send every sample/every period" pattern, it confirmed in code a design that genuinely reduces both mbox traffic and how often M55 wakes up. It also verified on real hardware how blocking `k_msgq_get()` with `K_FOREVER` serves a different purpose from Lab 14's finite timeout, and how that blocking call, combined with Zephyr's automatic idle/WFI behavior, produces a wait that is "genuinely resting" with no extra configuration. Explicitly entering an SoC-level low-power mode (`CONFIG_PM`) was left as unverified territory.

**Real-hardware verification complete.** The initial state send, event sending and TFT color/EVENTS updates on threshold crossings in both directions, commands `1 <celsius>`/`2`, and the fact that no mbox traffic occurs at all while the temperature stays stable near the threshold — all of this was confirmed working correctly on real hardware.

## Notes

- Lab 13/14 (`13_uart_bridge`, `14_heartbeat_watchdog`) — the source of the `uart_poll_in()` polling command-intake pattern this lab reuses, the criteria for choosing between `atomic_t` and `k_mutex`, and the original untagged single-message-kind design.
- Lab 12 (`12_env_logger`, or the corresponding lab name) — the source of the AHT20 raw I2C driver code and the I2C0 pinctrl verification process this lab reuses.
- Lab 11 (`11_display_basic`, or the corresponding lab name) — the source of the ST7789V3 raw-SPI driver this lab reuses as-is, along with the detailed explanation of the level shifter and SPI0 pin-sharing constraints.

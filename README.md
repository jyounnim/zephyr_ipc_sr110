# Astra SR110 M4↔M55 IPC Lab Curriculum

*[한국어](README_kr.md)*

A 16-lab, hands-on curriculum for learning Inter-Processor Communication (IPC) between the Cortex-M55 and Cortex-M4 cores of the Synaptics **Astra SR110** (board: `sr100_rdk`) — an asymmetric dual-core (AMP) SoC — using Zephyr RTOS's raw `mbox` driver API.

This README is the map for the whole curriculum. See each lab's own `doc/` folder for full detail.

## What this curriculum is (and isn't)

- **It uses the raw `mbox` API directly.** Higher-level IPC frameworks such as Zephyr's `ipc_service`/OpenAMP are out of scope. You call `mbox_send_dt()`, `mbox_register_callback_dt()`, `mbox_set_enabled_dt()` directly, with the goal of understanding how IPC actually works at the hardware level before reaching for anything higher-level.
- **Every lab follows the same build shape.** The M4 (remote) image is always built first, and its output is included in the M55 (host) build. As the labs progress, the message protocols and thread designs get more elaborate, but this build structure and the base project layout (`lab/` is M55, `lab/remote/` is M4) stay identical throughout.
- **All 16 labs (01–16) have been verified on real hardware** (an SR110 RDK board).
- **Each lab folder has only a `doc/` subfolder. There is no `README.md` at the lab folder's root.**
  - `doc/NN_topic_en.md` / `_kr.md`: the **lesson document**, describing the finished code as it stands today. It covers learning objectives, IPC concepts, pin connections, a devicetree/code walkthrough, and build/run instructions. It contains no debugging history — the final, working code is presented as if it had been designed this way from the start.
  - `doc/NN_topic_troubleshooting_en.md` / `_kr.md`: a **troubleshooting log** of the problems actually found and fixed (or, occasionally, still open) during hardware bring-up — present only for labs that had a real issue worth recording. It's kept separate from the lesson document on purpose: a learner meeting the lab for the first time should focus on "what is the correct design," and only reach for the troubleshooting notes if they hit a similar snag in their own setup.
  - **Every lab's documentation is self-contained.** Even when pin connections, level-shifter notes, or shared concepts were already explained in an earlier lab, the current lab's document explains them again in full rather than saying "see the earlier lab" — so that any lab can be worked through on its own.

## Hardware assumptions

- Board: `sr100_rdk` (Synaptics Astra SR110)
- M55: `sr100_rdk/sr100/m55` — the HOST core. In most labs, this is the side that kicks off the scenario.
- M4: `sr100_rdk/sr100/m4` — the CLIENT/REMOTE core. In most labs, this is the side that physically drives peripherals (GPIO, I2C sensors, SPI, etc.).
- M4 and M55 **physically share the I2C1 bus** on this board. Because of that, every lab that touches a device on I2C1 (LEDs, buttons, the accelerometer) disables `&i2c1`, `&gpio_exp0` (and its relevant child nodes) in the M55 overlay, leaving ownership of the bus to whichever core actually drives it (usually M4). The background for this is covered in detail in the [Lab 01 troubleshooting document](01_hello_ipc/doc/01_hello_ipc_troubleshooting_en.md).
- Separately, **I2C0** is a fully independent controller from I2C1, with its own pinctrl labels (`i2c0_ms_scl`/`i2c0_ms_sda`) on SoC pins GPIO3/GPIO4. Lab 12 connects a temperature/humidity/pressure sensor pair on this bus.
- **SPI0** is used directly by M55 from Lab 11 onward, to drive a TFT display. Enabling the SPI0 pins requires disabling M55's UART1 console (it shares GPIO23/24), so any later lab that wants to add a new SPI peripheral must check for this resource conflict first.
- **This SoC's actual design intent**: M4 is meant to stay powered on at all times as a lightweight, Always-On core watching cheap sensors, while M55 — which owns power-hungry blocks such as the Ethos NPU — is meant to sit in standby most of the time and wake up only on an event from M4 to do heavier work. Later labs in this curriculum (Lab 15 in particular) reproduce this real power-domain structure directly.
- Each core prints to its own independent UART console. It's worth keeping both consoles open side by side while working through a lab — see the section right below for exactly how to wire each one up.

## Connecting the M4 / M55 serial consoles

The two cores' consoles are reached in different ways. Wire up one of the methods below before starting a lab, and keep both consoles open at once while you work.

### M4 console

M4 is only reachable through an external USB-to-TTL converter (e.g. a CH340-based one).

- Header: **J24**
- Wiring: pin 13 = M4 **TX**, pin 14 = M4 **RX**, plus GND (cross-connect: the converter's TX goes to the board's RX, and the converter's RX goes to the board's TX)
- Baud rate: **230400 bps**, 8N1

### M55 console (two paths, physically one UART)

M55 can be reached either directly through the board's built-in USB-C connector, or through a header with an external USB-to-TTL converter, the same way as M4. **Important**: these are not two separate UARTs — they are two different physical paths (an onboard USB-serial bridge for the USB-C connector, an external converter header for the other) exposing the exact same M55 **UART1**. That's also why the baud rates differ (the bridge chip converts internally to 115200bps).

1. **Direct USB-C (simplest)**: just plug a cable into the board's **J14** USB-C connector. Baud rate is **115200 bps**.
2. **External USB-to-TTL converter**: same approach as M4, using the **J25** header — pin 13 = M55 **RX**, pin 14 = M55 **TX** (note the TX/RX pin numbers are swapped relative to M4's header). This way runs at **230400 bps**.

**From Lab 11 onward, any lab that uses SPI0 (the TFT) must disable M55's UART1 entirely in devicetree, because SPI0 shares GPIO23/24 with it. Since J14 and J25 are the same physical UART1 signal, disabling UART1 kills both paths — it is not a case of "J14 still works." From that point on, M55 has no serial console at all**, and the TFT screen is the only way to check M55's state (the design established in Lab 11). Each affected lab's own document restates this.

## Building (common to every lab)

Build from the root of the west workspace (the directory where `zephyr/` is visible), in this order. Replace `<N>_<lab_name>` with the lab folder's name.

```bash
# 1) Build the M4 (remote) image first
west build -p always -b sr100_rdk/sr100/m4 ./zephyr_ipc_sr110/<N>_<lab_name>/lab/remote -d m4

# 2) Build the M55 (host) image — this pulls in the M4 binary you just built
west build -p always -b sr100_rdk/sr100/m55 ./zephyr_ipc_sr110/<N>_<lab_name>/lab -d m55 \
    -DCONFIG_SR100_RELEASE_M4_RESET=y -DM4_BUILD="../m4"
```

`M4_BUILD` is resolved as a **relative path from the M55 build directory** (`-d m55`), not from your current working directory. With `m4/` and `m55/` as sibling directories under the workspace root, `../m4` is correct — pointing it at `./m4` fails silently (no CMake error at all) and leaves the M4 firmware out of the final image (M55 boots and runs fine; M4 never boots, as if it weren't there). If this repository is cloned into the workspace root under the name `zephyr_ipc_sr110/`, the build commands above can be copied as-is. Flashing the board follows whatever standard procedure your toolchain/board uses.

## IPC concepts, at a glance

Each lesson document explains a concept in depth the first time it appears. This section collects only the ideas that run through the entire curriculum — skim it before you start, and come back to it as you work through the labs.

**mbox (mailbox)** — a hardware block built into the SoC for inter-processor communication. When one core writes to a register, it raises a hardware interrupt (a "doorbell") on the other core. The mbox mechanism itself only carries the signal that "data has arrived" — the actual payload lives in a shared memory region defined in the devicetree.

**AMP (Asymmetric Multi-Processing)** — M4 and M55 don't share a single OS instance; each boots and runs its own independent Zephyr image. The two cores are, for all practical purposes, two separate computers sharing one chip — which is exactly why you need IPC instead of a function call or a shared global variable.

**ISR context and the no-blocking rule — the single most important rule in this curriculum** — a callback registered with `mbox_register_callback_dt()` runs in **interrupt service routine (ISR) context** the moment a message arrives. Inside an ISR you cannot make a blocking call that involves the scheduler (`k_msleep()` and friends), and — as hardware testing in Lab 07 proved — `mbox_send_dt()` itself can also block on this board's mbox backend, so it's banned from ISR context too, with **no exceptions**. Every lab in this curriculum follows this pattern as a result:

1. The mbox callback (running in ISR context) does nothing but copy the incoming message into a queue with `k_msgq_put(..., K_NO_WAIT)` and return immediately.
2. A separate worker thread waits on that queue with `k_msgq_get()`. When a message shows up, it's the worker thread — not the ISR — that does the real work (driving hardware, sending a reply, anything that might take time).

This "ISR → `k_msgq` → worker thread" pattern first appears in Lab 01, and every later lab reuses it in one shape or another. Why this rule is non-negotiable, and what actually happens when it's violated, is documented in detail in [Lab 03's troubleshooting notes](03_full_duplex_ping_pong/doc/03_full_duplex_ping_pong_troubleshooting_en.md) and, especially, [Lab 07's troubleshooting notes](07_echo_service/doc/07_echo_service_troubleshooting_en.md) (a real case where `mbox_send_dt()` in an ISR hung the entire system).

**Message protocol design** — the early labs (01–04) use the simplest possible message: one field, one value. Starting with Lab 06, a command-type field is used to multiplex several kinds of request/response onto a single channel. Later labs (especially Lab 16) take this further, unifying several message kinds into a single tagged-union (envelope) protocol.

**Periodic push vs. event-driven** — Lab 09 sends a sensor reading on a fixed schedule regardless of whether anything changed ("telemetry push"); Lab 10 only sends a message when a condition is met (a threshold is crossed — "event-driven"). Understanding the trade-offs between the two (bandwidth, latency, and how to handle a missed event) makes the design intent of later labs (15: Low Power Sync, 16: Capstone Gateway) much clearer.

**Implementing raw drivers directly (bypassing Zephyr subsystems)** — Lab 11 (ST7789V3 TFT) and Lab 12 (AHT20/BMP280) skip Zephyr's in-tree display/sensor subsystems and call low-level APIs (`spi_write_dt()`, `i2c_write()`, `i2c_read()`) directly to implement the device protocol themselves. There are two reasons for this: (1) when an in-tree driver requires devicetree properties or Kconfig symbols that haven't been confirmed to work on this board yet, implementing the protocol directly guarantees a known-working result instead of proceeding on an uncertain foundation, and (2) Zephyr's sensor subsystem assumes a fixed address baked into the devicetree, whereas a design that needs to scan several candidate addresses at boot to find whichever sensor is actually populated (like Lab 12's automatic BMP280 0x76/0x77 detection) is a better fit for a raw driver.

**Pin multiplexing trade-offs** — several pin resources on this SoC are contended for by more than one function. Just as enabling SPI0 for the TFT in Lab 11 required disabling M55's UART1 console (GPIO23/24), every time a new peripheral is enabled you need to check, at the devicetree overlay stage, what other function has to be given up. The same conflict needs to be re-checked whenever a later lab reuses SPI0/I2C0/I2C1.

**Always-On / Wake domain structure** — this SoC is designed around keeping M4 powered on at all times (Always-On), while the power-hungry M55 (and its Ethos NPU) normally sits in standby and wakes up only when M4 sends it an event. Lab 15 reproduces this structure directly: M4 continuously watches Lab 12's AHT20/BMP280 sensors, and wakes M55 over mbox only at the moment a threshold event fires (reusing Lab 10's event-driven pattern); M55 wakes up, shows the value on Lab 11's TFT, and goes back to standby. (An earlier design note had M55 own the sensor and M4 own the actuator instead — that reversed structure didn't match this chip's actual power/role design, so it was dropped from the curriculum.)

**Multi-sensor fallback and labeling the value's source** — Lab 12 uses two sensors that can both report temperature (AHT20, BMP280). It prefers the AHT20 reading (since it also provides humidity), and only falls back to BMP280's temperature when AHT20 is absent or its current read failed. The display always marks a fallback reading with something like `(bmp)` so the two sources are never confused.

**Two independent failure indicators: link loss vs. a bad reading** — Lab 12 shows "no message has arrived for a while (M4 is dead or unresponsive)" and "messages are arriving fine, but this particular sensor's reading failed on this cycle (M4 is alive; the sensor has a problem)" as **two separate status lines** on screen. The causes and the right response are completely different for each, so collapsing them into a single indicator would leave the user unable to tell which situation they're actually in.

**Active error recovery, and its limits** — when Lab 12's M4 sees a sensor read fail for a set number of consecutive attempts (5), it doesn't just report the error — it actively tries `i2c_recover_bus()` followed by re-initializing the sensor (aimed at the case where a brief power glitch left the sensor's configuration lost). Hardware testing showed, however, that once the I2C bus reaches a true SDA-stuck-low state, this software-only recovery cannot clear it — whether it can depends on whether the SoC's I2C controller driver implements a `recover_bus` callback, and whether `scl-gpios`/`sda-gpios` are present in devicetree, which remains an open issue documented in [Lab 12's troubleshooting notes](12_aht20_bmp280_logging/doc/12_aht20_bmp280_logging_troubleshooting_en.md). Recognizing the limits of software recovery — and documenting them honestly instead of hiding them — is itself part of what this curriculum teaches.

## Lab index

The original design notes for "Lab 13: SPI ADC Control Loop" and "Lab 15: Telemetry Hub" were dropped from the curriculum. Lab 13 (M55 owning the sensor, M4 owning the actuator — the reverse of every other lab) was an artificial scenario that didn't match this SoC's real design intent (M4 Always-On with lightweight sensors; M55 owning heavier, power-hungry blocks like the Ethos NPU and waking on demand). Lab 15 was just a straightforward merge of Lab 09 and Lab 12, which overlapped too much with the capstone character of Lab 16 to justify keeping separate. With those two dropped, the remaining labs were **renumbered 13–16**, and all four reuse **already-connected hardware** (Lab 12's I2C0 AHT20+BMP280, Lab 11's SPI0 ST7789V3 TFT, the I2C1 accelerometer from Labs 09/10, and the M4 console UART from Lab 01) rather than adding any new wiring.

| # | Title | Direction | New concept introduced |
|---|-------|-----------|-------------------------|
| 01 | [Hello IPC](01_hello_ipc/doc/01_hello_ipc_en.md) | M55 → M4 | mbox fundamentals, devicetree `mbox-consumer`, the ISR→msgq→worker-thread pattern |
| 02 | [Button Pong](02_button_pong/doc/02_button_pong_en.md) | M4 → M55 | Forwarding a device event to the host, `gpio-keys` polling mode |
| 03 | [Full-Duplex Ping-Pong](03_full_duplex_ping_pong/doc/03_full_duplex_ping_pong_en.md) | M55 ↔ M4 | Simultaneous bidirectional communication; the no-blocking-in-callback rule established |
| 04 | [Shared Counter](04_shared_counter/doc/04_shared_counter_en.md) | M55 → M4 | Synchronizing state via message passing vs. true shared memory |
| 05 | [Button Press Counter](05_button_press_counter/doc/05_button_press_counter_en.md) | M4 → M55 | Turning a repeated event into stateful, cumulative counting |
| 06 | [Structured Command](06_structured_command/doc/06_structured_command_en.md) | M55 ↔ M4 | A structured protocol multiplexing several request types via a command field |
| 07 | [Echo Service](07_echo_service/doc/07_echo_service_en.md) | M55 ↔ M4 | `mbox_send_dt()` is also banned from ISR context — the curriculum's final rule locked in |
| 08 | [Message Queue](08_message_queue/doc/08_message_queue_en.md) | M4 → M55 | Queuing a continuous message stream; sizing the queue depth |
| 09 | [Accel Telemetry](09_accel_telemetry/doc/09_accel_telemetry_en.md) | M4 → M55 | Periodic sensor telemetry; using Zephyr's sensor subsystem |
| 10 | [Threshold Event](10_threshold_event/doc/10_threshold_event_en.md) | M4 → M55 | Event-driven delivery, baseline calibration, commands + periodic work on one thread |
| 11 | [TFT Sensor Display](11_tft_sensor_display/doc/11_tft_sensor_display_en.md) | M4 → M55 | A raw SPI driver (custom `zds,st7789v` binding), the SPI0/UART1 pin conflict |
| 12 | [AHT20 + BMP280 Logging](12_aht20_bmp280_logging/doc/12_aht20_bmp280_logging_en.md) | M4 → M55 | Raw I2C address auto-scan, multi-sensor fallback, dual link-loss/sensor-error indicators, active recovery (and its limits) |
| 13 | [UART Bridge](13_uart_bridge/doc/13_uart_bridge_en.md) | M4 → M55 | Reusing the M4 console UART as a command channel, parsing text commands, relaying over mbox |
| 14 | [Heartbeat Watchdog](14_heartbeat_watchdog/doc/14_heartbeat_watchdog_en.md) | M4 → M55 | Periodic liveness signal, an M55-side watchdog state machine (timeout detection) |
| 15 | [Low Power Sync](15_low_power_sync/doc/15_low_power_sync_en.md) | M4 → M55 | Reproducing the Always-On/Wake power-domain structure, `K_FOREVER` deep-idle waiting |
| 16 | [Capstone Gateway](16_capstone_gateway/doc/16_capstone_gateway_en.md) | M4 ↔ M55 | A tagged-union (envelope) protocol unifying multiple sensors, commands, and the watchdog on one channel — the final integration lab |

## Recommended order

Numeric order is recommended. Each lab adds exactly one new concept on top of the pattern the previous labs established, and the labs below in particular mark the points where a core design principle of the whole curriculum gets locked in — don't skip these:

- **Lab 01**: mbox, devicetree, and the ISR/worker-thread pattern get established.
- **Lab 03**: why the no-blocking-in-callback rule matters, shown on real hardware.
- **Lab 07**: the curriculum's single most important rule — not even `mbox_send_dt()` may run in ISR context.
- **Lab 11**: when to implement a raw driver instead of an in-tree one; pin-multiplexing trade-offs.
- **Lab 12**: multi-sensor fallback, dual failure indicators, active recovery and its limits.
- **Lab 15**: the Always-On/Wake power-domain structure this SoC is actually built around.
- **Lab 16**: everything above, unified into one capstone protocol.

## Repository layout

```
zephyr_ipc_sr110/
├── README.md / README_kr.md   # This document (en / kr)
└── <N>_<lab_name>/
    ├── doc/                    # Lesson docs + troubleshooting notes (kr/en)
    └── lab/                    # M55 (host) sources, M4 (client) sources under remote/
```

## If you get stuck

Check that lab's troubleshooting document first. A handful of known issues recur across labs that share the same board (I2C1 bus sharing, the `M4_BUILD` relative path, the GPIO expander's `polling-mode` requirement, the SPI0/UART1 pin conflict, I2C0 SDA-stuck-low), and are usually documented down to the root cause wherever they were first discovered.

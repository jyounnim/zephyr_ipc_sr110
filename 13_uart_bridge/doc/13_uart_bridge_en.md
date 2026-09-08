# Lab 13: UART Bridge — Reusing the Console UART as a Command Channel

This document is self-contained: even if you've only read this lab's material, you should be able to follow it end to end, so parts already covered in earlier labs (especially Lab 11 and Lab 12) are repeated here in full wherever relevant.

## Learning Objectives

- Learn how to repurpose M4's console UART — until now used only to print boot/debug logs — as an operator input channel.
- Build the habit of checking a Zephyr driver API's **return value**, not just a Kconfig symbol, to verify whether a given UART instance actually supports a feature (a real case where `uart_irq_callback_set()` returned `-ENOTSUP`).
- Learn a practical fallback design — a dedicated thread's polling loop — for when interrupt-driven I/O isn't available.
- Learn how to multiplex two traffic patterns with different shapes — periodic telemetry and request/response — on a single mbox channel using a tagged union.
- Practice the full round trip: M4 sends a command, M55 responds, and the response is displayed back on M4's console.
- Practice the judgment that not every operator command needs to reach the other core — a purely local setting like turning M4's console log on/off is better handled directly on M4, without ever touching mbox.

## Connection to Earlier Labs, and What's New Here

This lab picks up Lab 12's hardware and code (the AHT20+BMP280 dual-sensor logger) unchanged. **No new wiring at all.**

- The AHT20 (temperature/humidity) and BMP280 (pressure) sensors on M4's I2C0 keep being sampled every 200ms exactly as in Lab 12, with 1-second averages and rapid-change events still sent to M55.
- The ST7789V3 TFT on M55's SPI0 keeps showing sensor values and connection status exactly as in Lab 11/12.
- **The one new thing** is a round-trip channel: an operator types a short command into M4's console UART — the very same UART that used to only print the boot log — and M4 relays it to M55, which replies.

## Why Reuse the Console UART Instead of Adding a New One

Every lab in this curriculum has connected a USB-to-TTL converter to the J24 header for M4's console from the very start (see "Pin Connections" below). Pins 13 (M4 TX) and 14 (M4 RX) of that connection have always been physically wired, but no lab so far has actually used M4's RX line (the input path from the converter into M4) — logs have only ever flowed M4 → PC (TX).

This lab is the first to use that already-connected RX line. The operator simply types into the same terminal window that has been showing the boot log all along, and presses Enter. This gets you two things:

1. No new hardware or pin connections are needed at all, since the wiring is already in place.
2. You get a concrete feel for the fact that this SoC's pins already double as candidates for several functions — first the I2C bus for sensors, then the SPI bus for the display, and now the console UART too.

## Core Concepts

### 1. Receiving UART RX — Polling, Not Interrupts

This lab originally tried applying the exact same "interrupt → `k_msgq` → worker thread" pattern used for mbox ISRs to UART RX as well (`CONFIG_UART_INTERRUPT_DRIVEN=y` + `uart_irq_callback_set()`). But on real hardware, **`uart_irq_callback_set()` returned `-ENOTSUP` (-134)** — meaning this board's `ns16550_uart0` instance simply does not support interrupt-driven RX at all. Turning on interrupt-driven mode via Kconfig is no guarantee that a specific UART instance's driver actually implements it — this was confirmed with real-hardware verification. (See `13_uart_bridge_troubleshooting_en.md` for how this was diagnosed.)

So this lab's final implementation is **polling-based**. A dedicated thread (`Uart_Cmd_Task`) calls `uart_poll_in()` in a short loop (10ms), reading a byte when one is ready and briefly sleeping and retrying when none is.

```c
while (1) {
    if (uart_poll_in(console_dev, &c) != 0) {
        k_msleep(UART_POLL_INTERVAL_MS);   /* no byte ready -- back off briefly */
        continue;
    }
    if (c == '\n' || c == '\r') {
        if (line_len == 0) { continue; }
        line_buf[line_len] = '\0';
        line_len = 0;
        /* parse_command() is called below */
    } else if (line_len < IPC13_LINE_MAX - 1) {
        line_buf[line_len++] = (char)c;
    }
    ...
}
```

With no interrupt involved, this code runs entirely in thread context from start to finish — there's no need for the mbox-ISR-style "just enqueue in the ISR" handoff to a separate thread. Byte collection, command parsing, and the `mbox_send_dt()` call to M55 all happen in sequence inside the single `Uart_Cmd_Task` thread. Responsiveness is bounded by the poll interval (10ms), but that's more than immediate compared to how fast a human types and presses Enter.

**Lesson**: when using a Zephyr driver API, remember that a Kconfig symbol only decides whether a code path is compiled in — whether a specific instance actually implements that feature has to be confirmed via the function's return value. Logging an `int` return value instead of silently discarding it, as done here, lets you quickly tell a hardware/wiring problem apart from a driver limitation the next time something doesn't respond.

### 2. Carrying Three Differently-Shaped Messages on One mbox Channel

Lab 06 first introduced the idea of a structured protocol that multiplexes several kinds of request/response using a command-type field. This lab pushes that idea one step further, tagging **two traffic patterns that differ in both direction and purpose** onto the same channel.

```c
enum ipc13_msg_type {
    IPC13_MSG_ENV  = 0,  /* M4 -> M55: periodic/event-driven sensor telemetry (same as Lab 12) */
    IPC13_MSG_CMD  = 1,  /* M4 -> M55: relayed operator command */
    IPC13_MSG_RESP = 2,  /* M55 -> M4: the response to that command */
};

struct ipc13_msg {
    uint8_t type;
    union {
        struct ipc13_env_payload  env;
        struct ipc13_cmd_payload  cmd;
        struct ipc13_resp_payload resp;
    };
};
```

M4 sends both `IPC13_MSG_ENV` (sensor values) and `IPC13_MSG_CMD` (operator commands) on `tx_channel`, and only receives `IPC13_MSG_RESP` (M55's response) on `rx_channel`. M55 does the mirror image: it receives both `IPC13_MSG_ENV` and `IPC13_MSG_CMD` on `rx_channel`, sorting them into two separate queues (`env_msgq`, `cmd_msgq`), and only ever sends `IPC13_MSG_RESP` on `tx_channel`. That this tx/rx channel pair can already be used bidirectionally was established back in Lab 07 (Echo Service); this lab simply adds a third message kind on top of that.

One new wrinkle: on M4, **two different threads — the sensor-sampling loop in `main()`, and `Uart_Cmd_Task` — can now both send on the same `tx_channel`.** A mutex (`tx_lock`) protects against their `mbox_send_dt()` calls interleaving.

```c
k_mutex_lock(&tx_lock, K_FOREVER);
mbox_send_dt(&tx_channel, &mbox_msg);
k_mutex_unlock(&tx_lock);
```

### 3. Validating Commands on M4 First, Whenever Possible

When the operator makes a typo, one option is to forward it all the way to M55 and have M55 reply "unknown command" — but this lab doesn't do that. If M4's `parse_command()` doesn't recognize the line as one of the three known commands (`1`, `2`, `3 <0|1>`), nothing is sent to M55 at all; M4 prints the error itself, right there.

> **Note**: These commands used to be text — `PING`/`STATUS`/`MODE <0|1>` — but were changed to single-digit numbers (`1`/`2`/`3 <0|1>`) partway through bring-up, to narrow down a "no response at all" problem on M4's console. This reduced the number of terminal/line-ending variables in play and made it easier to tell "a command-parsing problem" apart from "a UART RX pipeline problem" — it doesn't change anything about the IPC design itself.

```c
if (!parse_command(line_buf, &cmd)) {
    printk("[M4] unknown command: \"%s\" (try: 1=PING, 2=STATUS, 3 <0|1>=MODE, 4 <0|1>=LOG on/off)\n",
           line_buf);
    continue;
}
```

This lets M55's command-handling code assume "only ever-valid values arrive" and stay a plain `switch` statement, saving one IPC round trip. It's a deliberate choice made to show that which side filters bad input is a decision the designer gets to make.

### 4. Not Every Command Needs to Reach M55 — Command `4` (Log On/Off)

During real-hardware testing, the `periodic avg ...` log line kept printing every second, making it hard to visually track command input and responses. To fix this, command `4 <0|1>` was added — but unlike `1`/`2`/`3`, this command is **never forwarded to M55 at all**. It only turns M4's own console log on and off, so there's no reason for M55 to be involved.

```c
static bool handle_local_command(const char *line)
{
    int arg;

    if (sscanf(line, "4 %d", &arg) == 1 && (arg == 0 || arg == 1)) {
        atomic_set(&periodic_log_enabled, arg);
        printk("[M4] periodic log %s (M4-local -- not sent to M55)\n", arg ? "ON" : "OFF");
        return true;
    }
    return false;
}
```

Once `uart_cmd_task_entry()` finishes a line, it checks `handle_local_command()` first, before trying `parse_command()` (commands meant for M55). If it was handled locally, that's the end of it — no mbox send happens at all. This demonstrates a design question one step beyond item 3 ("M4 filters bad input first"): *does this command need to reach the other core in the first place?* `periodic_log_enabled` only decides whether M4's `main()` sensor loop prints `LOG_INF("periodic avg ...")`; it has no effect at all on the sensor telemetry M4 sends to M55 (`send_env()`).

### 5. Command List

| Command | Argument | Action | Forwarded to M55? |
|---|---|---|---|
| `1` | none | PING — M55 replies `"PONG"` | Yes |
| `2` | none | STATUS — M55 replies with the latest sensor reading + connection state as text (e.g. `T:23.5C H:45.2% P:1013.2hPa CONN:OK ERR:0x0`) | Yes |
| `3 0` / `3 1` | 0 or 1 | MODE — turns an extra status line at the bottom of the TFT off (0) / on (1) | Yes |
| `4 0` / `4 1` | 0 or 1 | LOG — turns M4 console's `periodic avg ...` log off (0) / on (1) | **No (M4-local)** |

Entering `3 1` makes a line reading `SEQ:<latest message number> UP:<M55's uptime in seconds>` appear at the bottom of M55's TFT; `3 0` makes it disappear again. Note that this line is only redrawn **when the next sensor telemetry message arrives**, so the change on screen lags the command by up to about a second, not instantly — a deliberate simplification to keep the screen-refresh logic simple.

Entering `4 0` immediately silences the `periodic avg ...` log line, so the `[M4] forwarded ...` / `[M55] ...` response lines for commands `1`/`2`/`3` are easy to follow without interruption. `4 1` turns it back on. The sensors themselves keep being read every 200ms and keep being sent to M55 regardless — this has zero effect on the TFT refresh or rapid-change event detection. It purely controls "screen noise" on M4's console.

### 6. How a Response Makes It Back to M4's Screen

When M55 sends `IPC13_MSG_RESP` on tx_channel, M4's mbox rx callback receives it, puts it on `resp_msgq`, and `Resp_Print_Task` waits on that queue and prints it once it arrives.

```c
printk("[M55] %s%s\n", resp.text, resp.ok ? "" : " (rejected)");
```

`printk()` is used instead of `LOG_INF()` so the operator's reply visually stands out among the timestamped `[00:00:00.000,000] <inf> ...` log lines around it.

## Architecture

```
[Operator terminal] --one line of text--> [M4 console UART RX]
                                        |
                          uart_poll_in() (Uart_Cmd_Task thread, 10ms period)
                                        |
                              handle_local_command()?
                                   /              \
                            if "4 <0|1>"          otherwise (1/2/3)
                        handled on M4 directly        |
                     (never forwarded to M55)  only if parse_command() accepts it
                                                       v
                          mbox tx_channel --IPC13_MSG_CMD-->
                                                                  [M55 rx_cb (ISR)]
                                                                          |
                                                                     cmd_msgq
                                                                          |
                                                                     Cmd_Task (thread)
                                                                          |  handles 1/2/3 (PING/STATUS/MODE)
                                                                          v
                          <--IPC13_MSG_RESP-- mbox tx_channel <----------+
                                        |
                              M4 rx callback (ISR)
                                        |
                                  resp_msgq
                                        |
                              Resp_Print_Task (thread)
                                        |
                                        v
                          [shown on operator terminal as "[M55] ..."]

(separately, exactly as before)
[AHT20/BMP280] --every 200ms--> [M4 sensor loop] --IPC13_MSG_ENV--> [M55 Display_Task] --> [TFT]
```

## Pin Connections (the full set of wiring actually used in this lab — identical to Lab 11/12, no new wiring)

### AHT20 + BMP280 Combo Module (M4, I2C0)

| Module pin | Connects to | Notes |
|---|---|---|
| VCC | 3.3V | |
| GND | GND | |
| SDA | SoC GPIO3 (I2C0 SDA) | `i2c0_ms_sda`, internal pull-up enabled |
| SCL | SoC GPIO4 (I2C0 SCL) | `i2c0_ms_scl`, internal pull-up enabled |

### ST7789V3 TFT (M55, SPI0)

| Panel pin | Connects to | Notes |
|---|---|---|
| VCC | 3.3V | |
| GND | GND | |
| SCL/CLK | SPI0 CLK | |
| SDA/MOSI | SPI0 MOSI | |
| CS | SPI0 CS | |
| RES/RST | SoC GPIO17 (J24 pin 3) | **Level shifter required** — the SoC GPIO is 3.3V, which may not match some panels' RST/DC input thresholds, so this goes through a level shifter |
| DC | SoC GPIO18 (J24 pin 4) | Same as above, through the level shifter |
| BLK (backlight) | 3.3V or a separate GPIO | A fixed 3.3V connection also works |

**Caution**: turning on SPI0 requires turning off M55's only UART (UART1, shared with GPIO23/24). The J14 USB-C connector and the J25 header are not two separate UARTs -- they are two different physical paths (an onboard USB-serial bridge for J14, an external converter header for J25) exposing the exact same UART1 signal. So disabling UART1 silences BOTH J14 and J25 -- from this lab onward, M55 gives up its serial console entirely, and **the TFT screen is the only way to check M55's state** (the design established in Lab 11, see the Notes section). This lab's `lab/prj.conf` already sets `CONFIG_UART_CONSOLE=n` and `CONFIG_CONSOLE=n`, and the `LOG_INF`/`printk` calls left in the M55 source are kept only for reading later if the console is ever re-enabled for debugging -- they don't actually go anywhere right now.

### Operator Command Input (M4 console UART, no new wiring)

| Target | Connection |
|---|---|
| M4 console (USB-to-TTL converter) | J24 header pin 13 = M4 TX, pin 14 = M4 RX, common GND (converter TX↔board RX, converter RX↔board TX, crossed) — 230400bps, 8N1 |

This connection is exactly the same one every lab has already used to view M4's boot log. This lab is the first to let you actually send text from the PC to M4 through that converter — i.e., typing directly into the terminal program on the PC end of that converter.

## Devicetree Configuration

This lab's M4/M55 devicetree overlays are **identical to Lab 12's** (there's nothing to change since there's no new hardware). M4's console UART (`ns16550_uart0`) is already designated `zephyr,console`/`zephyr,shell-uart` in the base board devicetree, so it's used as-is with no extra overlay entries.

`lab/remote/boards/sr100_rdk_sr100_m4.overlay` (I2C0):

```dts
#include <zephyr/dt-bindings/i2c/i2c.h>

&ipc0 {
    shared-memory-size = <0x400>;
};

&i2c0_ms_scl {
    bias-pull-up;
};

&i2c0_ms_sda {
    bias-pull-up;
};

&i2c0 {
    status = "okay";
    pinctrl-0 = <&i2c0_ms_scl &i2c0_ms_sda>;
    pinctrl-names = "default";
    clock-frequency = <I2C_BITRATE_STANDARD>;
};
```

`lab/boards/sr100_rdk_sr100_m55.overlay` (SPI0/TFT, disabling I2C1/camera):

```dts
#include <zephyr/dt-bindings/gpio/gpio.h>

&ipc0 {
    shared-memory-size = <0x400>;
};

&i2c1 { status = "disabled"; };
&gpio_exp0 { status = "disabled"; };
&ov02c10 { status = "disabled"; };
&ns16550_uart1 { status = "disabled"; };

&spi0 {
    #address-cells = <1>;
    #size-cells = <0>;
    status = "okay";
    pinctrl-0 = <&spi_mstr_mosi &spi_mstr_miso &spi_mstr_clk &spi_mstr_cs>;
    pinctrl-names = "default";

    st7789v_disp: st7789v@0 {
        compatible = "zds,st7789v";
        reg = <0>;
        spi-max-frequency = <4000000>;
        reset-gpios = <&gpioa 17 GPIO_ACTIVE_LOW>;
        dc-gpios = <&gpioa 18 GPIO_ACTIVE_HIGH>;
        width = <240>;
        height = <280>;
        x-offset = <0>;
        y-offset = <20>;
    };
};
```

M55's `CMakeLists.txt` also extends `DTS_ROOT` so it can find the custom `zds,st7789v` binding, exactly as in Lab 11/12:

```cmake
list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
```

## prj.conf

`lab/remote/prj.conf` (M4) — identical to Lab 12's:

```
CONFIG_MBOX=y
CONFIG_I2C=y
CONFIG_LOG=y
CONFIG_LOG_DEFAULT_LEVEL=3
CONFIG_PRINTK=y
CONFIG_MAIN_STACK_SIZE=2048
```

Command intake is handled via `uart_poll_in()` (see "Core Concepts" item 1 above), so no extra UART Kconfig symbol is needed — the `CONFIG_SERIAL` already on by default for the console is enough.

`lab/prj.conf` (M55) — identical to Lab 12's:

```
CONFIG_MBOX=y
CONFIG_SPI=y
CONFIG_GPIO=y
CONFIG_MAIN_STACK_SIZE=4096
CONFIG_UART_CONSOLE=n
CONFIG_CONSOLE=n
CONFIG_LOG=y
CONFIG_LOG_DEFAULT_LEVEL=3
CONFIG_PRINTK=y
```

## How to Build

```bash
# 1) Build the M4 (remote) image first
west build -p always -b sr100_rdk/sr100/m4 ./zephyr_ipc_sr110/13_uart_bridge/lab/remote -d m4

# 2) Build the M55 (host) image
west build -p always -b sr100_rdk/sr100/m55 ./zephyr_ipc_sr110/13_uart_bridge/lab -d m55 \
    -DCONFIG_SR100_RELEASE_M4_RESET=y -DM4_BUILD="../m4"
```

## Running It and Checking the Results

1. Open M4's console (J24, 230400bps). M55 has no serial console from this lab onward (see the Caution note above), so there is no separate M55 console to open -- check M55's state on the TFT screen instead.
2. Flash both images to the board and boot.
3. Confirm M55's TFT shows the title "LAB13 BRIDGE" along with connection state (WAIT→OK), sensor state (SENSOR OK), and T/H/P values, exactly as in Lab 12.
4. In M4's console window, type each of the following and press Enter:
   - `4 0` → confirm `[M4] periodic log OFF (M4-local -- not sent to M55)` prints, and `periodic avg ...` no longer appears afterward (this makes it much easier to watch command responses)
   - `1` → confirm `[M55] PONG` prints on M4's console
   - `2` → confirm a response like `[M55] T:23.5C H:45.2% P:1013.2hPa CONN:OK ERR:0x0` prints (actual values will vary)
   - `3 1` → confirm `[M55] MODE 1 OK` prints, and a `SEQ:... UP:...s` line appears at the bottom of the TFT the next time sensor telemetry updates (up to ~1 second later)
   - `3 0` → confirm that line disappears again
   - `9` (an undefined command) → confirm `[M4] unknown command: "9" (...)` prints immediately on M4's console, and nothing is sent to M55 (M55's log should show no reaction at all)
   - `4 1` → confirm `periodic avg ...` starts printing again

   If typing any digit produces not even an `unknown command` message on M4's console, the problem lies upstream of command parsing — somewhere between the terminal and `uart_poll_in()`. Check that your terminal program's flow control is set to `none` and that a line ending (CR or LF) is actually being transmitted.

## Summary

Without adding any new sensor or display, this lab shows that new IPC concepts — filtering out hardware features that turn out to be unsupported via real-hardware verification and falling back to polling, carrying several message kinds with different directions and purposes on one channel, designing a request/response round trip, and judging that not every command needs to reach the other core — can all be layered on top of already-available hardware (the dual sensors on I2C0, the TFT on SPI0, and M4's console UART). The next lab (Lab 14, Heartbeat Watchdog) follows the same principle: no new wiring.

**Real-hardware verification complete**: the M4→M55 round trip for commands `1`/`2`/`3 <0|1>`, and the M4-local handling of `4 <0|1>`, have all been confirmed working correctly on real hardware.

## Notes

- The `MODE` command's effect on screen is applied at the next sensor telemetry update (up to ~1 second later) — if instant feedback is needed, this could be changed to force one extra screen refresh right after M55 sends the `MODE` response.
- The process of discovering, on real hardware, that the console UART doesn't support interrupt-driven RX, and switching to polling as a result, is documented separately in `13_uart_bridge_troubleshooting_en.md`.

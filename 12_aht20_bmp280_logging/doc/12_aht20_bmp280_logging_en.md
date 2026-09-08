# Lab 12: Environment Sensor Logging (M4 AHT20+BMP280 → M55 → ST7789V3 color TFT)

M4 reads an AHT20 (temperature/humidity) + BMP280 (pressure) combo module over I2C and sends
the values to M55, which renders them live on a 1.69" 240x280 ST7789V3 color TFT panel wired
over SPI. If Lab 11 was "one sensor, assume it always works," this lab is the first to design
for **two sensors, where either one might be absent or stop responding partway through**.

This document is written so this lab alone is enough to build, wire, and understand from start
to finish -- background it shares with other labs (Lab 09, Lab 11: the TFT hardware, the raw-SPI
driver structure) is spelled out again here rather than only referenced.

## Learning objectives

- Understand the pattern of scanning the I2C bus at boot to auto-detect a sensor's actual
  address, to cope with real-world modules whose address can vary.
- Understand combining two different sensors (AHT20/BMP280) into a single IPC message, with one
  falling back to the other when it's absent or failing.
- Learn how to design a protocol that combines a baseline "send a 1-second average
  periodically" behavior with an exception "send immediately on a sharp jump" behavior.
- Learn to treat **connection loss** (no mbox message arrives at all) and **sensor error** (mbox
  is fine, but one specific sensor's value can't be read) as two distinct failure modes, and
  show them separately on screen.
- Reuse the "sensor telemetry -> mbox -> ISR/queue/worker-thread" pattern established in Labs
  09/11, and the "drive an ST7789V3 TFT with raw SPI" pattern established in Lab 11.

## Connection to the previous lab

Lab 11 had M4 read a single onboard accelerometer and push it to M55's TFT. This lab reuses
Lab 11's M55-side TFT driving code (panel init, fonts, SPI chunked transfers) unchanged, and
rewrites the M4 side completely -- because instead of an onboard sensor it reads an **externally
wired AHT20+BMP280 combo module**, and instead of a single sensor/single value it handles
**two sensors, three values (temperature/humidity/pressure), and two transmission modes
(averaged + immediate alert)**.

## Core concepts

### 1. Why raw I2C instead of Zephyr's sensor subsystem

Zephyr provides a standard sensor subsystem (`sensor_sample_fetch()`/`sensor_channel_get()`)
along with in-tree drivers for AHT20/BME280. This lab deliberately skips that path and calls
`i2c_write()`/`i2c_read()`/`i2c_write_read()` directly instead, for two reasons.

1. **Boot-time address auto-scan is required.** Zephyr's sensor subsystem requires a sensor
   node to be declared at a **fixed address** in devicetree, like `aht20@38`. But this lab's
   requirement is "addresses can vary between real modules, so scan for them at boot" --
   which doesn't fit a fixed-address assumption. BMP280 in particular lands on either 0x76 or
   0x77 depending on how its SDO pin is wired.
2. **Avoiding uncertainty over driver Kconfig symbol names.** We couldn't be sure ahead of time
   exactly what symbols like `CONFIG_AHT20`/`CONFIG_BMP280` are called in the Zephyr version
   this project uses (the same kind of problem Lab 11 hit when `CFB_SHELL` turned out not to
   exist as a symbol at all) -- raw I2C removes this uncertainty entirely.

The tradeoff is that both sensors' init/measurement command sequences have to be implemented by
hand in application code, following each datasheet (`aht20_read()`, `bmp280_read()`, etc).

### 2. AHT20 protocol (fixed address 0x38)

AHT20 has no chip-ID register, so its presence is confirmed purely by "does anything ACK at
this address."

- Soft reset: send `0xBA`, wait 20ms.
- Init: send `0xBE 0x08 0x00`, wait 10ms.
- Measure: send `0xAC 0x33 0x00` -> wait 80ms -> read 6 bytes.
  - Byte 0: status byte (top bit set means "measuring busy" -> treated as an error).
  - Bytes 1-4 (partial): 20-bit raw humidity (`raw_hum / 2^20 * 100` = humidity %).
  - Bytes 4-6 (partial): 20-bit raw temperature (`raw_temp / 2^20 * 200 - 50` = temperature °C).

Both `%` and `°C` are scaled directly to milli-unit (x1000) integers, with no floating point.

### 3. BMP280 protocol (address 0x76 or 0x77)

- Presence is confirmed by reading the chip-ID register (`0xD0`) and checking for `0x58`
  (BMP280) or `0x60` (BME280 -- register-compatible, only humidity differs).
- 24 bytes of calibration coefficients (`0x88`-`0x9F`, little-endian) are read into
  `dig_T1..T3`, `dig_P1..P9` -- these are factory-calibrated per chip and are required to
  convert raw readings into actual temperature/pressure.
- `ctrl_meas` (`0xF4`) is set to `0x27` (temperature/pressure oversampling x1, normal mode) and
  `config` (`0xF5`) to `0x00` (internal standby/filter left off, since we poll on our own 200ms
  schedule anyway) to start sampling.
- Raw 20-bit pressure and temperature are read from `0xF7`-`0xFC` and converted with Bosch's
  standard 64-bit integer compensation formula (`bmp280_compensate_temp()`,
  `bmp280_compensate_pressure()`).

> **Caution**: this compensation formula and command sequence are copied straight from the
> public datasheet, and this specific integration has not been verified on real hardware yet.
> If pressure comes out far outside ~950-1050 hPa, or temperature is off by 1-2 degrees from a
> reference thermometer, suspect the `dig_*` coefficient reads first.

### 4. Combining two sensors into one value (fallback)

This module can measure temperature from both AHT20 (temp+humidity) and BMP280
(pressure+its own temp). This lab's rule is:

- **If AHT20 is present and answering normally** -> use AHT20's temperature (the source that
  also gives humidity takes priority).
- **If AHT20 is absent, or present but fails to read this cycle** -> use BMP280's temperature
  instead. The screen shows the source in this case, e.g. `T:23.5C(bmp)`, so it's clear the
  value came from BMP280 rather than AHT20.
- **Humidity** has no alternate source -- only AHT20 can measure it. If AHT20 is absent or
  failing, the screen distinguishes `H:N/A` (never present) from `H:ERR` (was present, now
  failing).
- **Pressure** likewise has no alternate source -- only BMP280 can measure it.

### 5. 200ms sampling -> 1-second average, plus immediate alerts on a sharp jump

M4 reads both sensors every 200ms (per the original request: "scan sensors in 200ms
increments"). Every 5th sample (200ms x 5 = 1s), it computes the average over that window and
sends it as an `IPC12_REASON_PERIODIC` message. Averaging filters out directionless noise, but
has the downside of delaying notice of a genuinely fast temperature/pressure change (e.g. a door
opening and the room temperature suddenly shifting) by up to 1 second. So on every single 200ms
sample, this lab also compares against the last value actually reported to M55:

- If temperature deviates by >= 0.5°C -> an `IPC12_REASON_TEMP_JUMP` message is sent
  immediately, separately.
- If pressure deviates by >= 50hPa -> an `IPC12_REASON_PRESSURE_JUMP` message is sent
  immediately, separately.

The comparison baseline is "the last value actually sent," not "the previous raw sample," so
that a slow drift accumulating gradually across many samples doesn't trigger a message on every
single tick (a slow drift is captured by the 1-second average anyway).

### 6. Reporting sensor read errors (a different failure from connection loss)

Neither AHT20 nor BMP280 is guaranteed to **keep answering normally just because it was found
at boot** -- a wire can wiggle loose, the I2C bus can pick up noise, or a sensor can stop
mid-measurement. This lab reports this situation **separately** from the mbox connection itself
dropping.

- `sensor_mask`: whether this sensor was found at boot (fixed once decided, never changes
  afterward).
- `error_mask`: whether this sensor's I2C read failed **on this specific 200ms tick** (updated
  every tick). The moment this value differs from what was last sent to M55 -- i.e. a sensor
  starts failing, or a failing sensor recovers -- an `IPC12_REASON_SENSOR_ERROR` message is sent
  immediately (the same "send right away" approach as the temperature/pressure jump alerts).

M55 shows these two pieces of information as **two completely separate lines** on screen.

- **Connection status** (top line): shows "LOST" if no mbox message arrives from M4 for 2
  seconds. Indicates whether M4 itself, or the mbox/IPC link, is alive.
- **Sensor status** (line below it): mirrors the `error_mask` of the most recently received
  message. It means M4 is alive and sending messages normally, but some of the sensor values
  inside those messages couldn't be read. Shown as one of four states: `SENSOR OK` (green),
  `AHT20 ERR`/`BMP280 ERR` (only one has failed, red), or `SENSOR ERR` (both have failed, red).

Merging these two into one indicator would make it impossible to tell "M4 died" apart from "M4
is fine but one sensor died," so they are deliberately kept as two lines.

### 7. Sensor recovery (why a plain retry isn't enough)

The first version simply retried a failed sensor every 200ms tick, but real-hardware testing
found "even after reconnecting the wiring, it keeps showing ERR." The cause is two real I2C
hardware behaviors that a plain retry alone doesn't solve.

1. **The I2C bus itself can get physically stuck.** If wiring is disconnected mid-transaction,
   the slave (sensor) can end up stuck holding the SDA line low. In this state, the bus does not
   free itself just because the wiring is reconnected, and every subsequent I2C transaction
   keeps failing -- a separate "bus recovery" procedure that clocks SCL a few extra times to
   force the slave to release the line is needed.
2. **A sensor that briefly loses power forgets its configuration.** If VCC was also
   disconnected while unplugging/replugging the wiring, both AHT20 and BMP280 reset to their
   power-on state and won't respond correctly again until their init sequence is run once more.

So this lab, once a given sensor has failed **5 consecutive ticks (about 1 second) or more**,
tries the following two steps in order, then resets its failure counter.

- Call `i2c_recover_bus(i2c_bus)` -- manually toggles SCL a few extra times to force a stuck
  bus free. Some I2C controllers don't implement this API, so a failure here is only logged and
  execution continues to the next step (not treated as fatal).
- Re-run that sensor's init sequence -- AHT20 via `aht20_init()` (soft reset + init), BMP280 via
  `bmp280_configure()` (re-writing the `ctrl_meas`/`config` registers) -- so configuration is
  restored even if power was briefly lost.

If failures continue even after this recovery attempt (e.g. the wiring is still disconnected),
another 5 ticks are counted and the same recovery is retried -- the design goal being "once the
wiring is fully restored, resume normal operation automatically within about 1 second."

## Architecture

```
M4 (CLIENT)                                    M55 (HOST)
─────────────                                  ─────────────
Boot: scan i2c0                                Boot: init SPI0 + zds,st7789v
  Confirm AHT20(0x38) ACK + soft-reset/init      Disable ns16550_uart1 (console given up)
  Confirm BMP280(0x76 or 0x77) chip ID +         Draw "LAB12 ENV" title
    load calibration coefficients                Show connection status icon (WAIT)
Loop (every 200ms):                             Show sensor status icon (OK)
  aht20_read()  -> on failure, error_mask |= AHT20
  bmp280_read() -> on failure, error_mask |= BMP280
  (send SENSOR_ERROR immediately if error state changed)
  (send TEMP_JUMP/PRESSURE_JUMP immediately on a detected jump)
  Every 5 ticks (1s): compute average -> send PERIODIC  ──mbox──▶ rx_cb (ISR)
                                                   k_msgq_put(K_NO_WAIT)
                                                          │
                                                   Display_Task (worker thread)
                                                     k_msgq_get(timeout 500ms)
                                                     Success: connection=OK,
                                                       update sensor status/T/H/P lines
                                                     Timeout accumulates 2s: connection=LOST
```

- **M4 (`lab/remote/src/main.c`, CLIENT)**: scans for AHT20/BMP280 on `&i2c0` at boot, then
  reads both sensors every 200ms and sends one of three kinds of `struct ipc12_env_msg` messages
  (average / jump alert / error alert) over mbox as appropriate.
- **M55 (`lab/src/main.c`, HOST)**: `rx_cb()` only enqueues the message and returns immediately
  (drawing to the TFT over SPI is blocking and can't run directly in an ISR -- the rule
  established in Labs 03/07/11). The `Display_Task` worker thread pulls values off the queue and
  updates the screen, and separately wakes every 500ms to check whether 2 seconds have passed
  since the last message.

## Pin wiring (hardware connections)

This lab needs two separate sets of wiring -- the **AHT20+BMP280 combo module** (read by M4) and
the **ST7789V3 TFT panel** (driven by M55, identical wiring to Lab 11).

### AHT20+BMP280 combo module (I2C0, M4)

| Signal | SoC pin | devicetree property |
|---|---|---|
| I2C0 SCL | SR110_GPIO3 | `i2c0_ms_scl` pinctrl (bias-pull-up applied) |
| I2C0 SDA | SR110_GPIO4 | `i2c0_ms_sda` pinctrl (bias-pull-up applied) |
| VCC | Voltage stated in the module's datasheet (usually 3.3V) | — |
| GND | common | — |

I2C0 sits on its own pin group (the SoC's LPS_GEAR1 mux slot), fully independent of the group
shared by SPI0/UART0/UART1, so Lab 11's SPI0-vs-console conflict doesn't occur here. It's also a
completely separate controller from I2C1 (the bus carrying the onboard accelerometer/GPIO
expander/camera), so this lab doesn't strictly need the "disable I2C1 on M55" step every other
lab does (kept in the overlay anyway for consistency across the curriculum).

AHT20+BMP280 combo modules mostly run on 3.3V. Check the exact operating voltage in your
module's datasheet, and if SR110's GPIO3/4 pins themselves run at a different logic level than
3.3V (a level mismatch), a level shifter may be needed here too, just as with Lab 11's TFT --
confirm the voltage before wiring on real hardware.

### ST7789V3 TFT panel (SPI0, M55) — identical to Lab 11

This part reuses the wiring already verified in Lab 11 exactly. It's laid out again here so this
lab can be wired from this document alone.

**⚠️ A level shifter is required.** SR110's SPI0/GPIO pads run at **1.8V I/O**, while a commonly
available ST7789V3 module's IOVCC is typically 3.3V, and the logic-high threshold that implies
(VIH ≈ 0.7 x IOVCC ≈ 2.31V) is above what a 1.8V signal can reach. Driving it directly produces
**a persistent black screen with no SPI-level error at all**. All five of the following signals
need to go through a bidirectional level shifter.

| Signal | Direction | Notes |
|---|---|---|
| SCLK | M55 -> panel | continuously toggling clock |
| MOSI | M55 -> panel | |
| CS | M55 -> panel | native hardware CS, not a separate GPIO |
| RST | M55 -> panel | |
| DC | M55 -> panel | |

Confirmed working level shifter: **TXS0108E** (auto-direction-sensing). It isn't a perfect fit
for a fast, continuously-toggling signal like SCLK, so `spi-max-frequency` is kept at a
conservative 4MHz. MISO doesn't need to be wired at all -- this panel is write-only.

| Signal | SoC pin | Header location | devicetree property |
|---|---|---|---|
| SPI0 CLK | SR110_GPIO22 | J25 (Left 20pin) | `spi_mstr_clk` pinctrl |
| SPI0 MOSI | SR110_GPIO23 | J25 (Left 20pin) | `spi_mstr_mosi` pinctrl (shared with M55 UART1 TX) |
| SPI0 MISO | SR110_GPIO24 | J25 (Left 20pin) | `spi_mstr_miso` pinctrl (no wiring needed) |
| SPI0 CS | SR110_GPIO21 | J25 (Left 20pin) | `spi_mstr_cs` pinctrl (native hardware CS) |
| RESET | SR110_GPIO17 | J24, pin 3 | `reset-gpios = <&gpioa 17 GPIO_ACTIVE_LOW>` |
| DC | SR110_GPIO18 | J24, pin 4 | `dc-gpios = <&gpioa 18 GPIO_ACTIVE_HIGH>` |
| VCC | 3.3V (level shifter's HV rail, panel side) | — | — |
| GND | common | — | — |

Since SPI0 takes over M55's console (UART1, shares GPIO23/24), this lab also **gives up M55's
console** just like Lab 11, and uses the TFT screen itself as the output channel.

## devicetree configuration

### M4 overlay (`lab/remote/boards/sr100_rdk_sr100_m4.overlay`)

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

I2C0 starts as `status = "disabled"` with no default pinctrl-0, so both enabling the bus and
assigning the pin group have to be spelled out explicitly here. Sensor child nodes (like
`aht20@38 { ... }`) are deliberately not declared -- addresses are found by a boot-time scan,
which doesn't fit the fixed-address assumption of devicetree child nodes.

### M55 overlay (`lab/boards/sr100_rdk_sr100_m55.overlay`)

```dts
&i2c1 { status = "disabled"; };
&gpio_exp0 { status = "disabled"; };
&ov02c10 { status = "disabled"; };

&ns16550_uart1 {
	status = "disabled";
};

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

`zds,st7789v` is not a standard Zephyr binding -- it's the custom binding defined in Lab 11, and
this lab copies that same definition
(`lab/dts/bindings/display/zds,st7789v.yaml`) verbatim. For the devicetree compiler to find it,
`lab/CMakeLists.txt` extends `DTS_ROOT` with this app's own path *before*
`find_package(Zephyr...)` runs.

```cmake
list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
```

Disabling I2C1 (owned by M4) is the common pattern across every lab in this curriculum, but
since this lab's sensors are on I2C0, M55 has no real reason to touch I2C1 -- it's kept anyway
for consistency with the other labs.

### prj.conf

M4's (`lab/remote/prj.conf`) doesn't use the sensor subsystem, so `CONFIG_I2C` alone is enough.

```
CONFIG_MBOX=y
CONFIG_I2C=y
CONFIG_LOG=y
CONFIG_LOG_DEFAULT_LEVEL=3
CONFIG_PRINTK=y
```

M55's (`lab/prj.conf`) is identical to Lab 11 -- since it doesn't use the in-tree display
driver, it needs only SPI/GPIO, no
`CONFIG_DISPLAY`/`CONFIG_CHARACTER_FRAMEBUFFER`/`CONFIG_ST7789V`.

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

Why `CONFIG_UART_CONSOLE=n`/`CONFIG_CONSOLE=n` are needed: `ns16550_uart1` is disabled in
devicetree, but if the board defconfig's default `CONFIG_UART_CONSOLE=y` is left as-is, it
conflicts with `SERIAL_HAS_DRIVER=n` and fails the build at the Kconfig step. (Do NOT turn off
`CONFIG_SERIAL=n` directly -- this SoC's Kconfig unconditionally y-selects
`SERIAL_SUPPORT_ASYNC` from `SOC_SR100_M55`, which then conflicts with `SERIAL=n` and breaks the
build a different way.)

## Building

Run from the root of the west workspace (the directory where `zephyr/` is visible).

```bash
# 1) Build the M4 (remote) image first
west build -p always -b sr100_rdk/sr100/m4 ./zephyr_ipc_sr110/12_aht20_bmp280_logging/lab/remote -d m4

# 2) Build the M55 (host) image -- this pulls in the M4 binary you just built
west build -p always -b sr100_rdk/sr100/m55 ./zephyr_ipc_sr110/12_aht20_bmp280_logging/lab -d m55 \
    -DCONFIG_SR100_RELEASE_M4_RESET=y -DM4_BUILD="../m4"
```

`M4_BUILD` is resolved as a relative path from the M55 build directory (`-d m55`). If `m4/` and
`m55/` are sibling directories under the workspace root, `../m4` is correct.

## Running and checking the result

This lab also can't use M55's console, so check it through **M4's console only**, wired via an
external USB-to-TTL adapter (J24 pins 13/14, 230400bps). M4's console prints the boot-time
sensor scan results (`AHT20 found at 0x38`, `BMP280/BME280 found at 0x76`, etc.) and subsequent
jump/error event logs.

Right after boot, the TFT shows a "LAB12 ENV" title in yellow, then updates like this every time
a message arrives from M4.

```
LAB12 ENV
[■] OK          <- connection status (green/gray/red icon + text)
[■] SENSOR OK   <- sensor status (green: OK / red: AHT20 ERR, BMP280 ERR, or SENSOR ERR)
T:23.5C         (red; "T:23.5C(bmp)" or "T:ERR" if AHT20 absent/failing)
H:45.2%         (green; "H:N/A" or "H:ERR" if AHT20 absent/failing)
P:1013.2hPa     (cyan; "P:N/A" or "P:ERR" if BMP280 absent/failing)
```
<img width="323" height="483" alt="image" src="https://github.com/user-attachments/assets/ed9d0001-aba0-4451-a5c2-41493b72a8cf" />

If you deliberately wiggle or disconnect the sensor module's wiring (e.g. briefly disconnecting
the SDA line), you can watch the corresponding sensor's value switch to `ERR` within a few
hundred milliseconds -- this fast, per-sensor notification, distinct from the 2-second
connection timeout, is the core behavior this lab demonstrates.

## Summary

- Since real hardware modules can have different I2C addresses depending on part variation or
  wiring, scanning at boot can be more robust than hardcoding a fixed address in devicetree.
- When multiple sensors can measure the same physical quantity (temperature, here), a fallback
  design that substitutes one for another on failure improves availability -- but the screen
  must always indicate which sensor a value actually came from, to avoid confusion.
- Combining "periodic averaging" with "immediate alert on a sharp jump" produces a system that
  is stable against noise while still reacting quickly to real changes.
- **A dropped connection** and **a connection that's up but can't read a value** are different
  failures, and showing them differently to the user speeds up troubleshooting.

## Items needing confirmation (pre-hardware-verification)

1. **BMP280 compensation formula**: the integer-math formula from the public Bosch datasheet was
   copied as-is, but this specific integration was not hardware-verified. If values come out
   abnormal, check the `dig_*` coefficient reads in `bmp280_compensate_temp/pressure()` first.
2. **AHT20/BMP280 I2C address auto-scan**: AHT20 only probes its fixed address (0x38), and
   BMP280 only tries the two candidates 0x76/0x77. If a module uses a different address, add it
   to the `bmp280_candidates[]` array.
3. **Sensor-error determination criteria**: the current logic judges an error purely from
   whether the I2C transaction itself succeeded or failed on each 200ms tick. It does not catch
   a case where the value is read successfully but is obviously implausible (e.g. pressure
   coming back as 0) -- if this is observed during real-hardware testing, the determination
   logic may need to be strengthened.
4. **`i2c_recover_bus()` does not clear a "SDA Stuck Low" state (confirmed on real hardware)**:
   deliberately cutting and reconnecting the sensor's power to reproduce this showed that once
   this SoC's `i2c_dw` I2C controller driver detects a fully stuck bus (`SDA Stuck Low`), this
   lab's `i2c_recover_bus()` call plus sensor re-init is not enough to recover from it. The root
   cause analysis and ideas for what to try next are written up in the
   [troubleshooting document](12_aht20_bmp280_logging_troubleshooting_en.md). Lighter glitches
   from briefly wiggling the wiring are still recovered by this retry logic.

## Verified on real hardware (2026-09-08)

The full setup described in this document (I2C0 auto-scan, 200ms sampling / 1-second averaging,
immediate temperature/pressure jump alerts, sensor read-error detection with recovery after 5
consecutive failed ticks, and M55's separate connection-status/sensor-status display) has been
confirmed working on real hardware. Item 4 above (the extreme case of a fully stuck I2C bus)
remains an open issue -- see the troubleshooting document for details.

---

If you run into trouble → see
[this lab's troubleshooting document](12_aht20_bmp280_logging_troubleshooting_en.md) or
[Lab 11's document](../11_tft_sensor_display/doc/11_tft_sensor_display_en.md) (most TFT-related
issues are wiring/level-shifter related and identical here).

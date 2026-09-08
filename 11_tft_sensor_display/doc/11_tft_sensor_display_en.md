# Lab 11: TFT Sensor Display (M4 accelerometer → M55 → ST7789V3 color TFT)

M4 periodically reads the onboard MC3419 accelerometer and sends the values to M55, which
renders them live on a 1.69" 240x280 ST7789V3 color TFT panel wired over SPI. Labs 01-10 all
confirmed their values through a console log; this lab is the first where **M55 gives up its
own serial console and uses a physical display as its output instead**.

## Learning objectives

- Experience, on a real SoC where a single physical pin is multiplexed across several
  peripherals (SPI, UART, I2C, etc.), the hardware constraint that **enabling one function can
  force you to give up another**, and practice deciding how to work around it.
- Understand the "raw SPI driver" pattern: driving a display controller with a custom
  devicetree binding plus direct `spi_write_dt()` calls, instead of going through Zephyr's
  in-tree Display/CFB subsystem.
- Understand how a SPI controller's hardware limits (FIFO depth) have to be reflected in
  software design (transfer chunk size).
- Reuse the "sensor telemetry -> mbox -> ISR/queue/worker-thread" pattern established in
  Lab 09 unchanged, only swapping what the receiving side does with the data -- from logging
  to rendering on a display.

## Connection to the previous lab

Lab 09 had M4 read the accelerometer and send it to M55, which only logged the values to its
console. This lab reuses M4's sensor-reading logic completely unchanged from Lab 09, and only
changes what M55 does with the data -- from "log it" to "draw it on the TFT." In other words,
the IPC protocol and M4's code are a direct rerun of Lab 09; everything genuinely new in this
lab lives in how M55 drives the display.

The original curriculum draft had Lab 11 as M55 -> M4 -> SSD1306 (I2C OLED). The panel actually
on hand turned out to be SPI-based rather than I2C, and the user subsequently decided to swap
both the sensor/display role split (M4 = sensor, M55 = display) and the panel itself (SSD1306
-> a color ST7789V3 TFT), arriving at the design in this lab.

## Core IPC / hardware concepts

### 1. Pin multiplexing and the "enable one, give up the other" constraint

Many of this SoC's GPIO pins multiplex several peripheral functions onto a single physical pad.
This lab runs into exactly this combination:

| Physical pin | Overlapping functions |
|---|---|
| SR110_GPIO23 | SPI0 (SPI_MSTR) MOSI / M55 console (UART1) TX |
| SR110_GPIO24 | SPI0 (SPI_MSTR) MISO / M55 console (UART1) RX |

For M55 to drive the TFT over SPI0, pinctrl has to claim these two pins for SPI0 -- and the
moment it does, `ns16550_uart1` (M55's console) can no longer function physically. In this
situation there are roughly two options:

1. Move the console to a different physical UART/pin group (this SoC's M4 already uses UART0
   as its own console, so if M55 also claimed UART0, the two cores would now conflict over
   UART0 instead -- not an option given this curriculum's structure).
2. Give up the console entirely, and replace whatever information it carried with a different
   output channel (in this lab, the TFT screen).

This lab takes option 2 -- `Display_Task` rendering the received values on screen effectively
replaces this lab's "console log."

### 2. Why raw SPI instead of Zephyr's in-tree display driver

Zephyr ships a standard in-tree display driver, `"sitronix,st7789v"`, normally paired with
`CONFIG_DISPLAY` + `CONFIG_CHARACTER_FRAMEBUFFER` (cfb). This lab started down that path too,
but the Zephyr version this project uses (4.4.1) requires a `mipi-mode` devicetree property on
that binding, and getting it right kept breaking the build (see this lab's troubleshooting
notes for the details).

Instead, this lab ports the approach from a separate project that had already been hardware-
verified on this exact board (`jyounnim/zephyr_display-sr110`'s `06_TFT_ST7789V3` lab): skip
the in-tree driver entirely, define a minimal **custom devicetree binding (`zds,st7789v`)**
that only carries wiring info (SPI bus/CS, RESET/DC pins, panel geometry), and have the
application code implement the ST7789 command protocol directly with `spi_write_dt()`.

Trade-offs of this approach:

- **Pro**: not at the mercy of an in-tree driver's devicetree requirements shifting between
  Zephyr versions; the command sequence is plainly visible and understandable directly in the
  application code.
- **Con**: you lose standard framebuffer APIs like cfb (automatic line wrapping, font
  management, etc.) -- basic operations like filling a rectangle or drawing a character have
  to be implemented by hand (`st7789_fill_rect()`, `st7789_draw_char()` in this lab).

### 3. SPI hardware FIFO depth and transfer chunk size

SR110's SPI0 hardware FIFO is only 8 bytes deep. A single `spi_write_dt()` call sending more
than 8 bytes needs a mid-transfer FIFO-refill interrupt to complete, and that refill doesn't
happen correctly on this hardware -- the call reliably times out (`-ETIMEDOUT`) instead. So
this lab's `st7789_send()` chunks every transfer to 8 bytes or fewer:

```c
#define ST7789_CHUNK_BYTES 8

static int st7789_send(int dc_value, const uint8_t *data, size_t len)
{
	gpio_pin_set_dt(&dc_spec, dc_value);
	while (len) {
		size_t chunk = MIN(len, ST7789_CHUNK_BYTES);
		/* ... spi_write_dt() one chunk at a time ... */
		data += chunk;
		len -= chunk;
	}
	return 0;
}
```

This is a constraint of this specific SoC's SPI controller -- porting this code to a different
board means re-checking this value against that board's own SPI FIFO size.

## Architecture

```
M4 (CLIENT)                              M55 (HOST)
─────────────                            ─────────────
Enable mc3479 node (overlay)             SPI0 + zds,st7789v overlay
device_is_ready()                        ns16550_uart1 disabled (console given up)
sensor_attr_set(ODR)      ─┐
sensor_attr_set(FULL_SCALE)│  init (once)   Once at boot:
                            ┘               st7789_reset()
Loop (every 500ms):                         st7789_init() (full power/gamma sequence)
  sensor_sample_fetch()                     Draw "LAB11 ACCEL" title on the TFT
  sensor_channel_get() x3 (X/Y/Z)
  msg = {x, y, z, seq++}
  mbox_send_dt(&tx_channel, &msg) ──mbox──▶ rx_cb (ISR)
                                             k_msgq_put(K_NO_WAIT)
                                                    │
                                             Display_Task (worker thread)
                                               k_msgq_get()
                                               Update SEQ/X/Y/Z on screen via
                                               st7789_fill_rect()/draw_string()
```

- **M4 (`lab/remote/src/main.c`, CLIENT)**: identical to Lab 09. Opens the `mc3479` node, sets
  ODR/full-scale range once at startup, then reads X/Y/Z every 500ms and sends them in a
  `struct ipc11_accel_msg`.
- **M55 (`lab/src/main.c`, HOST)**: `rx_cb()` only enqueues the message and returns immediately
  (drawing to the TFT over SPI is a blocking operation that takes real time, so it can't run
  directly in an ISR -- the rule established in Labs 03/07). A separate `Display_Task` worker
  thread pulls values off the queue and draws them.

## Pin wiring

The MC3419 accelerometer M4 reads is **already on the board** and needs no external wiring
(same as Lab 09). The only new wiring in this lab is the **external ST7789V3 TFT panel**
connected to M55.

### ⚠️ A level shifter is required

SR110's SPI0/GPIO pads run at **1.8V I/O**. A commonly available ST7789V3 module's IOVCC is
typically 3.3V, and the logic-high threshold that implies (VIH ≈ 0.7 x IOVCC ≈ 2.31V) is above
what a 1.8V signal can reach. Driving the module directly produces **a persistent black screen
with no SPI-level error at all** (the SPI bus mechanics complete fine, but the panel never
recognizes the logic levels as valid). All five of the following signals need to go through a
bidirectional level shifter.

| Signal | Direction | Notes |
|---|---|---|
| SCLK | M55 -> panel | continuously toggling clock |
| MOSI | M55 -> panel | |
| CS | M55 -> panel | native hardware CS, not a separate GPIO |
| RST | M55 -> panel | |
| DC | M55 -> panel | |

Confirmed working level shifter: **TXS0108E** (auto-direction-sensing). It isn't a perfect fit
for a fast, continuously-toggling unidirectional signal like SCLK, though (it can be unreliable
above roughly 1-2MHz depending on wiring/parasitic capacitance), so this lab keeps
`spi-max-frequency` at a conservative 4MHz. A dedicated unidirectional buffer (74LVC245,
74AHCT125, etc.) would be the more correct choice. MISO doesn't need to be wired at all -- this
panel is write-only, the MCU never needs to read data back from it.

### Pin mapping

| Signal | SoC pin | Header location | devicetree property |
|---|---|---|---|
| SPI0 CLK | SR110_GPIO22 | J25 (Left 20pin) | `spi_mstr_clk` pinctrl |
| SPI0 MOSI | SR110_GPIO23 | J25 (Left 20pin) | `spi_mstr_mosi` pinctrl (shared with M55 UART1 TX) |
| SPI0 MISO | SR110_GPIO24 | J25 (Left 20pin) | `spi_mstr_miso` pinctrl (no wiring needed; shared with M55 UART1 RX) |
| SPI0 CS | SR110_GPIO21 | J25 (Left 20pin) | `spi_mstr_cs` pinctrl (native hardware CS) |
| RESET | SR110_GPIO17 | J24, pin 3 | `reset-gpios = <&gpioa 17 GPIO_ACTIVE_LOW>` |
| DC | SR110_GPIO18 | J24, pin 4 | `dc-gpios = <&gpioa 18 GPIO_ACTIVE_HIGH>` |
| VCC | 3.3V (level shifter's HV rail, panel side) | — | — |
| GND | common | — | — |

RESET/DC use J24 pins 3/4 -- the same header as M4's console, but physically different pins
from M4's console (pins 13/14), so there's no conflict.

## devicetree configuration

### M4 overlay (`lab/remote/boards/sr100_rdk_sr100_m4.overlay`)

Same as Lab 09 -- just re-enables the onboard accelerometer.

```dts
&ipc0 {
	shared-memory-size = <0x400>;
};

&mc3479 {
	status = "okay";
};
```

### M55 overlay (`lab/boards/sr100_rdk_sr100_m55.overlay`)

Disabling I2C1 (owned by M4) is the common pattern across every lab in this curriculum; what's
new here is the SPI0/TFT setup and disabling the console (UART1).

```dts
&i2c1 { status = "disabled"; };
&gpio_exp0 { status = "disabled"; };
&ov02c10 { status = "disabled"; };

/* SPI0 takes over the console (UART1) pins, so disable it explicitly */
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

`zds,st7789v` is not a standard Zephyr binding -- it's defined by this lab itself
(`lab/dts/bindings/display/zds,st7789v.yaml`). For the devicetree compiler to find it,
`lab/CMakeLists.txt` extends `DTS_ROOT` with this app's own path *before*
`find_package(Zephyr...)` runs:

```cmake
list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
```

### prj.conf

M4's (`lab/remote/prj.conf`) is identical to Lab 09. M55's (`lab/prj.conf`) doesn't need
`CONFIG_DISPLAY`/`CONFIG_CHARACTER_FRAMEBUFFER`/`CONFIG_ST7789V` at all since it isn't using
the in-tree driver -- just SPI and GPIO.

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

Why `CONFIG_UART_CONSOLE=n`/`CONFIG_CONSOLE=n` are needed: `ns16550_uart1` is disabled in the
devicetree, but the board defconfig's default `CONFIG_UART_CONSOLE=y` would then fail its
`SERIAL_HAS_DRIVER` dependency and abort the build at the Kconfig step entirely if left as-is.
(Conversely, do NOT turn off `CONFIG_SERIAL=n` directly -- this SoC's Kconfig unconditionally
y-selects `SERIAL_SUPPORT_ASYNC` from `SOC_SR100_M55`, which then conflicts with `SERIAL=n` and
breaks the build a different way.)

## Building

Run from the root of the west workspace (the directory where `zephyr/` is visible).

```bash
# 1) Build the M4 (remote) image first
west build -p always -b sr100_rdk/sr100/m4 ./zephyr_ipc_sr110/11_tft_sensor_display/lab/remote -d m4

# 2) Build the M55 (host) image -- this pulls in the M4 binary you just built
west build -p always -b sr100_rdk/sr100/m55 ./zephyr_ipc_sr110/11_tft_sensor_display/lab -d m55 \
    -DCONFIG_SR100_RELEASE_M4_RESET=y -DM4_BUILD="../m4"
```

`M4_BUILD` is resolved as a relative path from the M55 build directory (`-d m55`). If `m4/` and
`m55/` are sibling directories under the workspace root, `../m4` is correct.

## Running and checking the result

This lab can't use M55's console (see "Pin multiplexing" above), so check it through **M4's
console only**, wired via an external USB-to-TTL adapter (J24 pins 13/14, 230400bps -- see
[00_course_overview_en.md](../../00_course_overview_en.md#connecting-the-m4--m55-serial-consoles)
for the full wiring). It's expected that M4's console shows no output at all here, same as in
Lab 09 (M4 just keeps sending values in this lab too).

Right after boot, the TFT shows a "LAB11 ACCEL" title in yellow, then updates like this every
time a new value arrives from M4:

```
LAB11 ACCEL
SEQ:12 RX:12
X:-152        (red)
Y:203         (green)
Z:986         (yellow)
```

Moving or tilting the board updates the values on screen immediately. (As in Lab 09, these are
uncalibrated raw values -- a nonzero reading at rest is expected, due to gravity always loading
one axis.)

## Summary

- On an SoC where several peripherals share a single physical pin, enabling one feature you
  want can force you to give up another (especially a debug console) -- designing in a
  replacement output channel for whatever you gave up is the core lesson of this lab.
- When an in-tree driver's requirements keep shifting across Zephyr versions and getting in
  your way, a minimal custom devicetree binding plus a hand-written SPI driver covering only
  what you actually need can be a practical alternative.
- Low-level constraints like a SPI controller's hardware FIFO depth must be accounted for at
  the application level too (transfer chunk size).
- M4's IPC/sensor logic could be reused from Lab 09 completely unchanged -- a new lab doesn't
  always mean rewriting everything from scratch.

## Verified on real hardware (2026-09-08)

The full setup described in this document (M4 accelerometer -> mbox -> M55 raw-SPI ST7789V3
rendering, including the mbox callback -> queue -> `Display_Task` worker-thread wiring) has
been confirmed working on real hardware.

---

If you run into trouble → this lab's own troubleshooting document (to be written) or
[Lab 09's troubleshooting notes](../09_accel_telemetry/doc/09_accel_telemetry_troubleshooting_en.md)
(most accelerometer-related issues are the same).

# Lab 16: Capstone Gateway — Troubleshooting

This doc records the real problems hit while bringing Lab 16 up on hardware, and how each was found and fixed. The lecture doc (`16_capstone_gateway_en.md`) describes the already-finalized code; this one covers the pitfalls on the way there.

## 1. M55 build error — `DT_N_..._P_spi_max_frequency' undeclared`

### Symptom

The M55 build failed with:

```
.../m55/zephyr/include/generated/zephyr/devicetree_generated.h:14802:37:
error: 'DT_N_S_soc_S_spi_50315000_S_st7789v_0_P_spi_max_frequency' undeclared here (not in a function);
did you mean 'DT_N_S_soc_S_spi_50315000_S_st7789v_0_P_compatible_LEN'?
```

### Root Cause

Two things were missing at once.

1. **The custom devicetree binding file was missing entirely.** `lab/dts/bindings/display/zds,st7789v.yaml` — present in every lab since Lab 11, and the thing that makes the TFT's `compatible = "zds,st7789v"` node inherit `spi-device.yaml` (and therefore recognize standard SPI child properties like `spi-max-frequency`) — never got copied when this lab was written.
2. **The `DTS_ROOT` extension was missing.** M55's `lab/CMakeLists.txt` was also missing the `list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})` line (which must come before `find_package(Zephyr...)`, and is what lets the devicetree compiler find an app-local bindings directory in the first place).

This lab's M55 `CMakeLists.txt` was reused from the old Lab 18 stub (which used an SSD1306 I2C OLED via Zephyr's in-tree display driver) -- that stub never needed a custom binding at all, so this difference went unnoticed when reusing it. With both pieces missing, the devicetree compiler compiled the `zds,st7789v` node without its standard properties, so the `spi-max-frequency` macro was never generated, which then broke `SPI_DT_SPEC_GET()`'s macro expansion at compile time.

### Fix

Copied both files over from Lab 15:

- Added `lab/dts/bindings/display/zds,st7789v.yaml`.
- Added `list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})` to the top of `lab/CMakeLists.txt`.

**Lesson**: when a new lab reuses an earlier lab's custom (non-in-tree) devicetree binding, the `dts/bindings/` directory that binding requires, and the `CMakeLists.txt`'s `DTS_ROOT` setting, need to move over as a full set -- not just the `.c`/`.overlay` code. Looking only at the code and concluding "it's the same devicetree node, so copying the overlay is enough" is exactly what let the binding file itself go missing this time.

## 2. Documentation error — "M55's console is still visible over USB-C (J14) even with SPI0 on"

### Symptom

The docs said "turning on SPI0 requires turning off M55's UART1 console (shared with GPIO23/24) ... to view M55's console you must use the direct USB-C connection (J14, 115200bps)" -- but in reality, nothing comes out of J14 either.

### Root Cause

J14 (direct USB-C) and J25 (external USB-to-TTL converter header) are not two separate UARTs -- they are two different physical paths (through an onboard USB-serial bridge for J14, or through an external converter for J25) exposing the exact same **UART1** on M55. The base board's devicetree pin table itself labels `SR110_GPIO23`/`GPIO24` as "SPI0 MOSI/MISO, shared with M55 console (UART1) TX/RX," and M55 only has this one UART to begin with. So disabling UART1 with `&ns16550_uart1 { status = "disabled"; };` silences M55's log over both J14 and J25.

This was actually stated correctly from the start in the Lab 11 doc ("M55 gives up its serial console entirely, using the physical display as its output instead"). But when Lab 13's doc was written, a line reflexively got added to its "Running It and Checking the Results" section saying to keep "M55's console (J14 USB-C)" open -- and that incorrect line then got copied verbatim into Lab 14/15/16 and the curriculum overview doc. It went unnoticed through hardware verification because the TFT screen alone was always enough to check M55's behavior, so nobody actually needed to open M55's console.

### Fix

Corrected the relevant wording in `00_course_overview_kr.md`/`_en.md` and the Korean/English docs of `13_uart_bridge`, `14_heartbeat_watchdog`, `15_low_power_sync`, and `16_capstone_gateway` to: "J14 and J25 are the same physical UART1 signal -- disabling UART1 kills both. In any SPI0 lab, the TFT screen is the only way to check M55's state." **The code (devicetree/prj.conf) was correct from the start** -- only the documentation's description of "how to open M55's console" was wrong.

**Lesson**: on a board with both an onboard USB bridge and an external header, never assume "connector A doesn't work, so connector B still will" without first checking the devicetree pin table for whether they're really separate UART controllers. This error happened specifically because that check was skipped and an earlier lab's wording was copied reflexively instead.

### Real-Hardware Verification Result

After fixing both issues above, with only M4's console connected (M55's console never existed to begin with), the following was confirmed:

- The ENV badge switches green (OK) / red (HIGH) as the temperature crosses the threshold.
- The MOTION badge turns red (ACTIVE) when the board is shaken, and returns to green (QUIET) once it stops.
- The watchdog badge stays green (OK) while heartbeats keep arriving.
- Commands `1 <celsius>`/`2 <milli-g>`/`3` are all handled locally on M4's console only (mbox unused).
- Command `4` pushes a STATUS to M55, and the STATUS line at the bottom of the TFT updates.

**Lab 16 fully verified.**

# Lab 01: Hello IPC — Troubleshooting Notes

This document records the actual problems encountered and how they were resolved during Lab 01's development and hardware validation. All fixes are already reflected in the current final code — this document exists purely as a reference record.

## Why the code includes a diagnostic ack

Lab 01 was originally designed to cover only one-way signaling, from M55 to M4 (bidirectional communication is formally covered in Lab 03). During development and validation, though, there was a period when the M4's physical console (a separate UART) hadn't been wired up yet. In that state, the M55 console alone gave no way to tell whether the M4 was actually receiving pings, or whether the LED0 toggle was actually succeeding.

So a temporary diagnostic path was added: every time the M4 receives a ping, it packages the result (success / failure / device-not-ready) into a `struct ipc_ack_msg` and sends it back to the M55, letting you infer the M4's state from the M55 console alone. The M55 console distinguishes the following cases:

- `[ACK] seq=N M4 alive, LED0 toggle OK` → The M4 is receiving pings correctly, and the LED0 GPIO write (the I2C transaction) succeeded. If the physical LED still doesn't light up despite this, the problem is most likely in the physical wiring/hardware.
- `[ACK] seq=N M4 alive, but LED0 device NOT READY` → The M4 is alive and receiving pings, but the `led0` (gpio_exp0) device itself was never ready at boot. Look into I2C1/gpio_exp0 initialization failures (e.g. the PCA6416A not actually responding).
- `[ACK] seq=N M4 alive, LED0 toggle FAILED ret=N` → The M4 is alive and the device is ready, but the GPIO write (the I2C transaction) itself failed with error code N. Suspect the I2C bus state or the wiring.
- **No ack log at all** → Either the M4 failed to boot, it isn't receiving pings at all (an IPC-level problem), or there's a problem with the M4→M55 mbox channel specifically. This is the most fundamental failure mode, so check it first.

For context: before this diagnostic path was added, the earlier version of the code had the M4's `main()` `return` immediately if `led0` failed to become ready/configured. In that version, the mbox callback never even got registered — which meant there was no way, from the M55 side, to tell "IPC works but only the LED failed" apart from "the M4 never even got that far." The code was later changed so the IPC/ack path stays alive even when `led0` fails, narrowing the root cause down to the four cases above.

This ack path started out as temporary diagnostic scaffolding, but it turned out to still be useful as a regression check even after the M4 console became directly viewable, so it was left in the final code. It has no effect on normal operation.

## Resolved Issue 1 — M4 and M55 physically share the same I2C1 bus

**Symptom**: The M55 console correctly prints `Ping sent, seq=N` every second, but LED0 — which the M4 is supposed to control — never blinks. A standalone M55-only LED blink sample works fine for both LED0 and LED1.

**Root cause**: Comparing `sr100_rdk_sr100_m4.dts` against `sr100_rdk_sr100_m55.dts` showed that both cores' `&i2c1` reference **exactly the same physical pins** (`i2c1_ms_scl_b`, `i2c1_ms_sda_b`) through pinctrl. In other words, the M4's I2C1 and the M55's I2C1 aren't separate buses at all — they're **the same physical I2C1 bus**, with a single `gpio_exp0` (a PCA6416A at address 0x20) hanging off it, driving LED0, LED1, and the button.

Both boards' base dts files have `&i2c1 { status = "okay"; ... }` enabled regardless of application code, so building M4 and M55 together meant **both cores tried to initialize the same physical I2C1 bus as master at boot, simultaneously**. This conflict appears to be what silently caused the M4's `gpio_pin_set_dt()` I2C write to fail (an M55-only build has no such conflict, which is why it worked correctly on its own).

**Fix**: Added `&i2c1 { status = "disabled"; };` to `lab/boards/sr100_rdk_sr100_m55.overlay`, so only the M4 — the core that actually drives the LEDs/button — owns this bus. This fix has been applied identically across the M55 overlays for all of Lab 01 through Lab 16 (safe to do, since none of the other labs touch i2c1 either).

> Note: this change also disables the `vcc_sd1` regulator (used for the SD card, `gpio_exp0 7`) and camera (`ov02c10`) power control (`gpio_exp0 8/13`) on the M55 side. That's harmless for these IPC labs since none of them use the SD card or camera, but keep it in mind if you ever repurpose this overlay for something else.

## Resolved Issue 2 — Disabling `&i2c1` alone isn't enough (M55 build error)

Applying only the fix from Issue 1 (`&i2c1 { status = "disabled"; };`) and then building M55 fails as follows:

```
FAILED: .../gpio_pcal64xxa.c.obj
.../zephyr/include/zephyr/device.h:96:41: error: '__device_dts_ord_23' undeclared here
```

**Root cause**: Disabling the parent node (`&i2c1`) in devicetree does not disable its **child node (`gpio_exp0`)** — a child node without its own explicit `status` keeps the default of `"okay"`. So the `nxp,pcal6416a` GPIO expander driver (`gpio_pcal64xxa.c`) is still treated as an "active instance" and included in the build. Inside that driver, `DT_BUS(node_id)` tries to reference the parent I2C1 bus's device handle — but since that bus is disabled, no such handle was ever generated, which is what produces the compile error.

**Fix**: Explicitly disable the child nodes too, in the M55 overlay:

```dts
&gpio_exp0 {
    status = "disabled";
};

&ov02c10 {
    status = "disabled";
};
```

(`ov02c10` is the camera sensor node present in the M55 base dts; it's disabled for the same reason.) This has been applied across all M55 overlays for Lab 01 through Lab 16.

## Resolved Issue 3 — What `M4_BUILD`'s relative path is resolved against

**Symptom**: The M55 appears to work fine, but the M4 seems to not even boot.

**Root cause**: The `-DM4_BUILD=...` value passed to `west build` to include the M4 image in the M55 build is resolved **relative to the M55 build directory (the directory given with `-d`, e.g. `m55/`), not the current working directory (CWD)** where the command is run. This comes from the following logic in `zephyr_srsdk/soc/syna/astra_sr/sr100/CMakeLists.txt`:

```
find_file(M4_ELF ... PATHS ${CMAKE_BINARY_DIR}/${M4_BUILD}/zephyr/ NO_DEFAULT_PATH)
```

With `m4/` and `m55/` as sibling directories, the correct value is **`../m4`**, not `./m4`. Passing `./m4` causes CMake to silently fail to find `M4_ELF` — with no error message at all, because of the `NO_DEFAULT_PATH` option — and `srsdk_image_generator.py` subsequently gets invoked without the `-m4_image` argument, so **the final flash image ends up with no M4 firmware in it whatsoever**. From the outside, this looks exactly like "the M55 works fine, only the M4 fails to boot," which makes the actual cause hard to pin down.

**Fix**: Pin the M55 build's argument to `-DM4_BUILD="../m4"`. This value is always correct as long as you follow this curriculum's standard build layout — M4 and M55 as sibling build directories (`-d m4`, `-d m55`).

## Lab 01 Final Validation — Complete

After the `M4_BUILD="../m4"` fix, a rebuild/reflash confirmed correct output on both the M55 and M4 consoles, along with the actual LED0 toggle.

- M55: `Ping sent, seq=N` + `[ACK] seq=N M4 alive, LED0 toggle OK`, printed correctly every second.
- M4: `[Toggle_Task] ping seq=N -> LED0 toggle ret=0`, printed correctly every second, with LED0 visibly blinking confirmed.

## Items That Needed Confirmation During Development (all now confirmed)

At the time the code was written, some parts were written based purely on devicetree/sample-pattern analysis, without an actual west/HAL build environment available. The following items were subsequently confirmed against real hardware builds/tests. All are now confirmed.

1. **The `CONFIG_GPIO_PCA95XX` symbol**: Actual build logs confirmed that `gpio_pcal64xxa.c` gets included in the build automatically, independent of this Kconfig symbol — it appears that when an `nxp,pcal6416a` node is present in devicetree with `okay` status, Kconfig auto-selects the driver (via something like `default y if DT_HAS_...`). `CONFIG_GPIO_PCA95XX` itself is likely not even a real symbol (presumably just ignored), but the M4-side LED behavior itself was confirmed working correctly on real hardware.
2. **The M55's default console UART**: Confirmed that the M55's default console is already set to `ns16550_uart1` at 230400 bps in the base dts, with no overlay needed.
3. **`DT_PATH(mbox_consumer)`**: Confirmed that the M55-side base dts also has an `mbox-consumer` node by default (`mboxes = <&ipc0 1>, <&ipc0 0>;` — it's expected/correct that the tx/rx order is mapped opposite to the M4's).
</content>

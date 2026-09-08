# Lab 12 troubleshooting: "SDA Stuck Low" doesn't auto-recover after a power reconnect

## Symptom

Deliberately cutting and reconnecting the sensor module's power to force an error condition
left both the M55 display and M4's log stuck showing `AHT20 ERR`/`BMP280 ERR` indefinitely,
with no automatic recovery.

```
[00:28:03.625,000] <err> i2c_dw: SDA Stuck Low on i2c@50312000
[00:28:04.026,000] <wrn> lab12_client: AHT20 failing repeatedly -- attempting bus/sensor recovery
[00:28:04.026,000] <err> i2c_dw: SDA Stuck Low on i2c@50312000   <- recurs right after recovery
```

## Root cause analysis

The `i2c_dw: SDA Stuck Low` log line comes from this SoC's Synopsys DesignWare I2C controller
driver (`i2c_dw`) checking the bus state on its own **before** starting a transaction. Since the
same error reappears immediately after this lab's `i2c_recover_bus()` call (invoked every 5
consecutive failed ticks), it appears that call isn't actually performing the hardware recovery
procedure (clocking SCL a few extra times to force a slave holding SDA low to release it).

For `i2c_recover_bus()` to have any real effect, two conditions need to hold:

1. This SoC's `i2c_dw` driver needs to actually implement the `recover_bus` callback.
2. (In a typical Zephyr implementation) the I2C controller's devicetree node needs
   `scl-gpios`/`sda-gpios` properties, so the pins can be temporarily switched to plain GPIO to
   manually generate clock pulses when needed.

This project's `&i2c0` overlay doesn't specify `scl-gpios`/`sda-gpios`, and whether this SoC's
`i2c_dw` driver actually implements that callback could not be confirmed from this environment,
since the driver source itself wasn't accessible here.

## Current state

- The recovery-attempt logic in `lab/remote/src/main.c` (calling `i2c_recover_bus()` plus
  re-initializing the sensor after 5 consecutive failed ticks) has been left in place -- it may
  still help if `recover_bus` turns out to be supported on this SoC, or in cases where power was
  only briefly interrupted and the bus never actually reached a fully stuck state.
- However, as reproduced this time, once the bus genuinely reaches an **SDA-stuck state**, this
  software-only recovery was confirmed not to clear it.

## Areas for further investigation (things to try next)

1. Check the `i2c_dw` driver source inside `zephyr_srsdk` (or this SoC's HAL) directly for
   whether it implements the `recover_bus` callback. If it does, find out which devicetree
   properties it requires (`scl-gpios`/`sda-gpios`, etc.) and add them to the `&i2c0` overlay.
2. If it isn't implemented, this failure mode cannot be resolved in software alone. Alternatives:
   - Add power sequencing on the sensor module side (e.g. a MOSFET on the sensor's power rail
     that the MCU can control), letting software fully power-cycle the sensor to physically
     reset the bus when needed.
   - Lower the I2C pull-up resistor values (e.g. 10kΩ -> 4.7kΩ or lower) as a hardware
     mitigation that stabilizes faster even in a low state (this doesn't fundamentally fix an
     already-stuck bus, though).
3. Narrow down the reproduction conditions further: comparing what happens when only power is
   cut/reconnected versus when only the I2C wiring (SDA/SCL) is disconnected/reconnected may
   help isolate whether the root cause is on the slave (sensor) side or the controller side.

## Conclusion

This session stopped investigation at this point. The application-level retry/re-init logic is
kept as-is, but genuine recovery from a fully stuck-SDA state depends on whether this SoC's I2C
controller driver supports `recover_bus`, which requires looking directly at the driver source
to confirm.

# Lab 05 Troubleshooting: Button Press Counter

This document summarizes issues that actually came up while working through Lab 05, along with their causes and fixes. Issues from the same family reappear here as in Labs 01 and 02, so also refer to the root-cause analysis in those labs' documents.

### Symptom: I2C1 bus conflict (M4/M55 boot failure, or `gpio_exp0` misbehaving)

The M4 and M55 physically share the same I2C1 bus (the SCL/SDA pins). If both cores try to initialize this bus at the same time, a bus-initialization conflict can occur.

**Cause**: `gpio_exp0` (the PCAL6416A GPIO expander chip) — which `user_button` (and LED0/LED1) hang off of — sits on the I2C1 bus, and both the M4 and M55 images try to initialize this bus by default.

**Fix**: In `lab/boards/sr100_rdk_sr100_m55.overlay`, `&i2c1` on the M55 side is disabled with `status = "disabled"`, so that only the M4 actually drives this bus. However, disabling the parent bus alone is not enough — the child nodes (`&gpio_exp0`, `&ov02c10`) still default to `status = "okay"`, causing a build error where `DT_BUS()` fails to find the disabled i2c1 handle. So the child nodes must also be disabled explicitly.

```dts
&i2c1 {
	status = "disabled";
};

&gpio_exp0 {
	status = "disabled";
};

&ov02c10 {
	status = "disabled";
};
```

See the [Lab 01 troubleshooting document](../../01_hello_ipc/doc/01_hello_ipc_troubleshooting_en.md) for the detailed root cause.

---

### Symptom: M4 boot log shows `interrupt configuration failed: -134`, pressing the button has no effect

```
[00:00:00.010,000] <err> gpio_keys: interrupt configuration failed: -134
[00:00:00.010,000] <err> gpio_keys: Pin 0 interrupt configuration failed: -134
```

`-134` is Zephyr's `-ENOTSUP`.

**Cause**: `user_button` hangs off `gpio_exp0` (a PCAL6416A, an I2C GPIO expander chip), but the `gpio_exp0` definition in the M4-side base devicetree has no `int-gpios` property (the M55-side definition does have `int-gpios = <&gpioa 3 GPIO_ACTIVE_LOW>;`, but it's moot since M55 disables i2c1 in this lab). The `gpio_pcal64xxa.c` driver is written so that `pin_interrupt_configure()` always returns `-ENOTSUP` for any instance without `int-gpios`, so the default interrupt-based `gpio-keys` mode simply cannot work on the M4 side of this board. This is the same issue first discovered in Lab 02.

**Fix**: `zephyr/dts/bindings/input/gpio-keys.yaml` has a boolean property called `polling-mode` for exactly this situation. It was added to the M4 overlay (`lab/remote/boards/sr100_rdk_sr100_m4.overlay`) to switch from interrupts to periodic polling (at the default `debounce-interval-ms` of 30 ms):

```dts
&buttons {
	polling-mode;
};
```

**Note**: `polling-mode` must always be applied to the `gpio-keys` **parent node** (`&buttons`). Attaching it to the individual button child node (`&user_button`) causes a devicetree binding validation error.

---

### Symptom: Mis-specifying the `M4_BUILD` path causes the M55 build to fail to find the M4 image

**Cause**: `M4_BUILD` is a **relative path** from the M55 build directory (`m55/`). Mistaking it for an absolute path, or a path relative to something else, causes the M55 build to fail to find the M4 binary.

**Fix**: If `m4/` and `m55/` are sibling directories under the workspace root, `-DM4_BUILD="../m4"` is correct. See the [Lab 01 troubleshooting document](../../01_hello_ipc/doc/01_hello_ipc_troubleshooting_en.md) for the detailed root cause.

```bash
west build -p always -b sr100_rdk/sr100/m55 ./zephyr_ipc_sr110/05_button_press_counter/lab -d m55 \
    -DCONFIG_SR100_RELEASE_M4_RESET=y -DM4_BUILD="../m4"
```

---

## Design Review — No Blocking Calls in mbox Callbacks (Confirmed: No Issue)

Checking this lab's structure against the principle established in Lab 03 ("no blocking calls in mbox callbacks") found no issues requiring a fix.

- **M55 side**: `mbox_rx_callback()` only logs and returns immediately (no blocking calls), so there's no problem.
- **M4 side**: Instead of the mbox callback, it's the input subsystem callback (`button_input_cb`, running in workqueue context) that only enqueues onto the message queue, while the actual mbox send is handled in a separate `Send_Task` worker thread — so this structure is already safe.

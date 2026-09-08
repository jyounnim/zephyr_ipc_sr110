# Lab 13 Troubleshooting: Console UART RX Interrupt Not Supported

## Symptom

When testing Lab 13's command intake feature on real hardware (typing `1` / `2` / `3 <0|1>` into M4's console to forward a command to M55), the console showed no response at all to any keystroke. The boot log printed `[Uart_Cmd_Task] started ...` normally, confirming the thread itself was running, but neither a command-handled log line nor an `unknown command` error ever appeared, no matter what was typed.

## Diagnosis

1. **Ruled out the terminal program itself**: TeraTerm is a normal bidirectional terminal, so the hypothesis "it's just a log viewer and never actually sends keystrokes" was set aside.
2. **Suspected flow control**: If TeraTerm's Setup → Serial port flow control is set to `hardware` and the board has never asserted CTS, TeraTerm can silently withhold transmission. This was tested by switching flow control to `none` and retrying — the symptom was identical, ruling this out too.
3. **Added temporary diagnostic instrumentation** to narrow the cause further:
   - Logged the return value of `uart_irq_callback_set()` via `LOG_WRN`.
   - Added an atomic counter (`uart_isr_byte_count`) incremented for every byte the RX ISR received, and printed it as `uart_rx_bytes=N` on the existing periodic log line.
4. **The decisive evidence** appeared in the next reboot's log:

   ```
   [00:00:00.007,000] <wrn> lab13_client: uart_irq_callback_set() returned -134 -- interrupt-driven RX may not be supported on this UART instance
   ```

   `-134` is `-ENOTSUP` under Zephyr's minimal libc. After that, `uart_rx_bytes` stayed at `0` no matter what was typed into the console. This did not mean bytes were failing to reach the UART FIFO or driver — it confirmed that **the RX interrupt callback itself is not supported on this particular UART instance**.

## Root Cause

The `ns16550_uart0` instance (M4's console UART) does not support interrupt-driven RX (`uart_irq_callback_set()` / `uart_irq_rx_enable()`) on this board/SoC combination. `CONFIG_UART_INTERRUPT_DRIVEN=y` only compiles that code path into the build — it does not guarantee that a given UART instance's driver actually implements it. Every earlier lab had only ever used this UART for TX (polling-based log output), so this limitation had never surfaced before.

## Fix

The interrupt-driven design was abandoned in favor of a polling approach: a dedicated thread (`Uart_Cmd_Task`) repeatedly calls `uart_poll_in()` on a 10ms interval. Command text was also changed from `PING`/`STATUS`/`MODE <0|1>` to single-digit numbers (`1`/`2`/`3 <0|1>`), so that if a similar issue ever recurred, it would be faster to tell "a parsing problem" apart from "an input-path problem." After this change, the command round trip (M4 → M55 → M4) was confirmed working correctly on real hardware.

## Lessons Learned

- Some of Zephyr's interrupt-driven UART functions (such as `uart_irq_callback_set()`) return an `int` — never ignore that return value. Others, like `uart_irq_rx_enable()`, return `void`, so not every failure can be caught in code; when a function does return a status, it's worth logging that value during early bring-up on real hardware, as was done here.
- A Kconfig symbol (`CONFIG_UART_INTERRUPT_DRIVEN=y`) means "this feature is compiled in and available to use," not "this feature works on this hardware." Even the same UART driver can support a different feature set depending on the instance (console UART vs. general-purpose UART).
- When interrupts aren't available, short-interval polling from a dedicated thread is a practical fallback. For something operating at human typing speed (a command console), the performance cost of polling instead of interrupts is negligible.

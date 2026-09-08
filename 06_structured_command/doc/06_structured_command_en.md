# Lab 06: Structured Command Message

This lab covers a **command-response pattern**: M55 alternates between `SET_LED` and `GET_STATUS` commands packed into a single `struct` message, sent every 2 seconds, while M4 identifies the command type, actually drives the LED accordingly, and replies with its current state (LED state + accumulated button-press count).

## Learning Objectives

- Unlike earlier labs that carried a single scalar value, understand how to multiplex several kinds of requests over one IPC channel using a **struct with a command-type field**.
- Implement a **request-response** flow directly on top of the mbox API, where M55 (the requester) sends a command and M4 (the responder) sends back the result.
- Learn how to handle, within a single protocol, both commands that require a reply (`GET_STATUS`) and commands that don't (`SET_LED`).
- Apply the principle established in Labs 03–05 — "the mbox callback must never block; do the real work in a worker thread" — to a more complex message-handling scenario.

## Connection to Previous Labs

| Lab | Message content | Direction | Pattern |
|---|---|---|---|
| 01 | A single `seq` | M55 → M4 | Simple ping |
| 02 | Button event | M4 → M55 | Simple notify |
| 03–05 | Counter value | Bidirectional | Value update + non-blocking callback |
| **06** | **Command struct (`cmd`, `led_id`, `led_state`) / status struct (`led0_state`, `led1_state`, `button_press_count`)** | **M55 → M4 (command), M4 → M55 (response)** | **Command/response multiplexing** |

In the previous labs, the message was always a single piece of data with a fixed meaning — "the counter value," for example. Starting with Lab 06, we extend this on the premise that **a message can be one of several kinds**: a `cmd` field identifies which kind, letting a single mbox channel carry a variety of requests. This is an extremely common pattern in real IPC design, and it lays the groundwork for the more elaborate protocols (variable-length messages, error codes, etc.) covered in later labs.

## Core IPC Concept: A Structured Command/Response Protocol

### Why design the command as a struct instead of a single value

In every lab so far, the data handed to mbox was just a single `uint32_t seq`, so the receiver never had to figure out "what am I supposed to do." Receiving the value meant doing exactly one predetermined thing. In a real system, however, one core can ask the other to do many different things (turn on an LED, report its status, read a sensor, reset, ...). Giving each of those its own mbox channel would make the channel count grow with the number of features.

So in practice, you place a **command-type field** at the front of the message and use a single channel to carry several kinds of requests. This lab's `ipc_command_msg` is exactly that structure.

```c
/* lab/include/ipc_common.h */
enum ipc_cmd_type {
	IPC_CMD_SET_LED    = 1,
	IPC_CMD_GET_STATUS = 2,
};

/* M55 -> M4 command */
struct ipc_command_msg {
	uint32_t cmd;       /* enum ipc_cmd_type */
	uint32_t led_id;    /* 0 = LED0, 1 = LED1 (only used by SET_LED) */
	uint32_t led_state; /* 0/1 (only used by SET_LED) */
};
```

The `cmd` field indicates the "kind" of this message, while the remaining fields (`led_id`, `led_state`) are **payload** whose meaning depends on the command type — and which may not be used at all for some commands. The receiver (M4) inspects `cmd` and branches on how to interpret the rest of the fields. This lets a single channel support two requests with entirely different meanings — `SET_LED` and `GET_STATUS` — at the same time. That is what we mean by **multiplexing one channel across several logical request kinds**.

### Why a command/response pattern like GET_STATUS is so common in IPC

`SET_LED` is "just give the instruction and you're done," but `GET_STATUS` only makes sense if you learn **what the current state actually is**. In other words, when a requester needs to query the other side's **current state**, a request alone isn't enough — a reply has to come back. This is the command/response (or request-reply) pattern, and it recurs constantly in IPC in situations such as:

- One core needs to check hardware state that only the other core knows about (in this lab: only M4 has the LED GPIOs and the button count)
- You need to confirm whether a command actually took effect (success/failure)
- The two cores need to synchronize their state

In this lab, M55 plays the **requester** role and M4 the **responder** role, cleanly separated. M55 has no direct way to know the state (the LEDs and button are physically wired to M4 only), so it sends `GET_STATUS` and passively waits for the result. M4 fills a `struct ipc_status_msg` with the real state it holds (`g_led0_state`, `g_led1_state`, `g_button_presses`) and sends it back as-is.

```c
/* M4 -> M55 reply (sent only in response to GET_STATUS) */
struct ipc_status_msg {
	uint32_t led0_state;
	uint32_t led1_state;
	uint32_t button_press_count;
};
```

One thing to note: the mbox layer in this lab does **not** guarantee, at the protocol level, the correlation between "a request" and "its response." M55 simply sends `GET_STATUS` and then "assumes" that the next message arriving in its rx callback is the status reply. This assumption holds only because `SET_LED` never generates a reply — if M4's protocol instead sent an acknowledgment (ack) for every single command, M55 would need an additional field, such as a request ID, to tell which reply belongs to which request. The structure here is deliberately the simplest possible form for teaching purposes; in a real product, handling this request-response correlation correctly is one of the central concerns of protocol design.

### Things to watch for when designing the message struct

Both structs in this lab consist entirely of `uint32_t` fields. That's not an accident — it's a deliberate design choice.

- **Fixed size**: mbox is a low-level API that copies a raw byte buffer as-is (`mbox_msg.data`, `mbox_msg.size`). If the sender and receiver disagree on the struct layout, `memcpy` fills in the wrong values. That's why `ipc_command_msg`/`ipc_status_msg` are defined once, in a single `ipc_common.h`, forcing both the M55 and M4 builds to **include the exact same definition**.
- **Alignment/padding**: When every field is a `uint32_t` laid out back-to-back, the compiler has no reason to insert padding bytes between fields (every field is naturally 4-byte aligned). Had the struct mixed `uint8_t` and `uint32_t`, differing compiler options or struct-packing settings between the M4 and M55 builds could shift the padding positions apart. This lab eliminates that risk entirely by keeping all field types uniform.
- **Receiver-side size validation**: both `mbox_rx_callback()` implementations check `if (data->size < sizeof(...))` before doing any `memcpy`. This guards against the peer sending a message of a different size (e.g., a different lab's image was flashed by mistake) or the IPC channel getting corrupted, so a bad size gets safely ignored rather than causing a buffer overread.

## Architecture / Code Walkthrough

### M55 (HOST, `lab/src/main.c`) — Requester

`main()` cycles through a 5-step state (`step % 5`) and sends one command every 2 seconds: `SET_LED led0=ON` → `SET_LED led1=ON` → `GET_STATUS` → `SET_LED led0=OFF` → `SET_LED led1=OFF` → (repeat). `send_cmd()` simply hands a `struct ipc_command_msg` straight to `mbox_send_dt()`, sending directly from the `main()` loop with no separate thread. The receive callback (`mbox_rx_callback`) validates the size of the `struct ipc_status_msg` M4 replies with and just logs it — it makes no blocking calls, so it is safe to run in the mbox ISR context.

### M4 (CLIENT, `lab/remote/src/main.c`) — Responder

This lab follows exactly the "callback enqueues and returns immediately; real work happens in a worker thread" structure established in Lab 03.

1. `mbox_rx_callback()` copies the received bytes into a `struct ipc_command_msg`, puts it on `cmd_msgq` via `k_msgq_put()`, and returns immediately. (ISR context, no blocking allowed.)
2. The `Cmd_Task` worker thread (`cmd_task_entry`) pulls commands one at a time with `k_msgq_get()` and branches on `cmd.cmd`.
   - `IPC_CMD_SET_LED`: selects LED0/LED1 via `cmd.led_id`, drives the actual GPIO with `gpio_pin_set_dt()`, and also updates the global state (`g_led0_state`/`g_led1_state`).
   - `IPC_CMD_GET_STATUS`: fills a `struct ipc_status_msg` with the global state (`g_led0_state`, `g_led1_state`, `g_button_presses`) and sends it back to M55 with `mbox_send_dt()`.
   - Any other, unrecognized `cmd` value is logged with `LOG_WRN()` and otherwise ignored (a minimal defense so that older firmware doesn't crash even if the protocol grows over time).
3. Button (`user_button`) input increments `g_button_presses` inside the Zephyr Input subsystem callback (`button_input_cb`). This value is carried back to M55 in the next `GET_STATUS` reply.

## Devicetree Configuration Notes

This lab's overlays carry forward the same two board issues already settled in earlier labs.

- **Disabling `&i2c1` on the M55 side** (`lab/boards/sr100_rdk_sr100_m55.overlay`): because M4 and M55 physically share the same I2C1 bus (which carries `gpio_exp0`, the PCA6416A, wired to LED0/LED1/the button), only M4 — the core that actually drives the LEDs/button — should own that bus. `&i2c1` and its child nodes (`&gpio_exp0`, `&ov02c10`) are set to `disabled` on the M55 side. See the [Lab 01 troubleshooting doc](../../01_hello_ipc/doc/01_hello_ipc_troubleshooting_en.md) for the root cause.
- **`&buttons { polling-mode; };`** (`lab/remote/boards/sr100_rdk_sr100_m4.overlay`): the `gpio_exp0` on the M4 side has no `int-gpios` wired up, so interrupt-based button detection fails with `-ENOTSUP(-134)`. Setting `polling-mode` on the parent node (`&buttons`, not the child `&user_button`) switches it to periodic polling (default debounce 30 ms). See the [Lab 02 troubleshooting doc](../../02_button_pong/doc/02_button_pong_troubleshooting_en.md) for the root cause.
- **`ipc0` shared-memory-size**: both the M4 and M55 overlays must set the same `shared-memory-size = <0x400>;`. If the two values differ, the shared-memory layout the mbox channel references gets misaligned, which can lead to build or runtime errors.

## Build Instructions

```bash
# 1) Build M4 (remote) first
west build -p always -b sr100_rdk/sr100/m4 ./zephyr_ipc_sr110/06_structured_command/lab/remote -d m4

# 2) Build M55 (host), including the M4 binary via M4_BUILD
west build -p always -b sr100_rdk/sr100/m55 ./zephyr_ipc_sr110/06_structured_command/lab -d m55 \
    -DCONFIG_SR100_RELEASE_M4_RESET=y -DM4_BUILD="../m4"
```

> **Note**: `M4_BUILD` is a path relative to the M55 build directory (`m55/`). If `m4/` and `m55/` are sibling directories under the workspace root, `../m4` is correct (see the [Lab 01 troubleshooting doc](../../01_hello_ipc/doc/01_hello_ipc_troubleshooting_en.md) for the full explanation).

M4 must be built first because the M55 build references the M4 ELF output through the `M4_BUILD` path and bundles it into the final image.

## Running and Verifying Results

Open each core's serial console at 230400bps 8N1 to observe.

- **M55 console**: a command log such as `CMD -> SET_LED led0=ON` cycles every 2 seconds, and on the `GET_STATUS` turn, M4's reply is printed as `STATUS <- led0=N led1=N button_presses=N`.
- **M4 console**: logs `SET_LED led_id=N state=N applied` and `GET_STATUS replied` are printed. You can visually confirm LED0/LED1 on the actual board turning on and off according to the commands. Pressing `user_button` is reflected in `button_presses` on the next `GET_STATUS` reply (since it's polling mode, the update lands after the debounce-interval-ms, 30 ms by default).

## Summary / Key Takeaways

- Placing a **command-type field** at the front of a message lets a single mbox channel multiplex several kinds of requests.
- The **command/response pattern** is essential whenever a requester needs to query state that only the other side knows; in this lab the roles are clearly split into M55 (requester) and M4 (responder).
- Keeping message struct field types uniform **prevents padding/alignment issues**, and sharing a single header (`ipc_common.h`) between both builds prevents layout mismatches.
- The receive callback must still **validate size, enqueue, and return immediately**; the actual command processing happens in a worker thread (an extension of the Lab 03 principle).

## Next Lab Preview

Up through this lab, M55 has never actually waited for M4's reply after sending a command — it simply moves on to the next command in what is essentially a fire-and-forget, asynchronous style. Lab 07 (Echo Service) verifies IPC reliability with a round-trip (echo) structure, where M4 sends back exactly the data M55 sent it — and in the process, one of the most important design rules in this entire curriculum gets nailed down through hands-on testing: **even `mbox_send_dt()` must never be called directly from inside the mbox callback (ISR context)**. Up to now the rule has been "the callback only enqueues; only the heavy work is deferred to a worker thread." Starting in Lab 07, you'll learn — through a real hang scenario — that this must extend to "the `mbox_send_dt()` call that sends the message back must, without exception, happen only in a worker thread."

---

If you run into problems → see [`06_structured_command_troubleshooting_en.md`](./06_structured_command_troubleshooting_en.md)

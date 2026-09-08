# Lab 13: UART Bridge — 콘솔 UART를 커맨드 채널로 재사용

이 문서는 이 랩 하나만 보고도 처음부터 끝까지 따라갈 수 있도록, 앞선 랩(특히 Lab 11, Lab 12)에서 설명한 내용도 필요한 부분은 다시 온전히 반복해서 설명합니다.

## 학습 목표

- 지금까지 "로그를 찍기만 하던" M4의 콘솔 UART를 조작자(operator) 입력 채널로 재사용하는 방법을 익힙니다.
- Zephyr 드라이버 API를 쓸 때, Kconfig 심볼 하나만 믿지 않고 **함수의 리턴값을 실제로 확인**해서 "이 UART 인스턴스가 이 기능을 정말 지원하는가"를 검증하는 습관을 실습을 통해 체감합니다 (`uart_irq_callback_set()`이 `-ENOTSUP`을 반환한 실제 사례).
- 인터럽트를 쓸 수 없는 상황에서, 전용 스레드의 폴링(polling) 루프로 대체하는 실용적인 대안 설계를 배웁니다.
- 하나의 mbox 채널 위에 "주기적 텔레메트리"와 "요청/응답(request/response)"이라는 성격이 다른 두 가지 트래픽을 태그된 유니온(tagged union)으로 함께 실어 보내는 방법을 배웁니다.
- M4가 보낸 커맨드에 M55가 응답하고, 그 응답이 다시 M4의 콘솔 화면에 표시되는 왕복(round-trip) 구조를 실습합니다.
- 모든 operator 커맨드가 상대 코어까지 갈 필요는 없다는 것을 실습합니다 — M4 콘솔 로그 on/off처럼 순전히 로컬한 설정은 M4에서 바로 처리하고 mbox를 아예 타지 않는 편이 맞다는 판단 기준을 세워봅니다.

## 이전 랩과의 연결, 그리고 이 랩에서 새로 추가되는 것

이 랩은 Lab 12(AHT20+BMP280 이중 센서 로깅)의 하드웨어와 코드를 그대로 이어받습니다. **새로운 배선은 전혀 없습니다.**

- M4의 I2C0에 물린 AHT20(온습도)과 BMP280(기압) 센서는 Lab 12와 완전히 동일한 방식으로 계속 200ms마다 읽히고, 1초 평균값과 급변 이벤트가 M55로 전송됩니다.
- M55의 SPI0에 물린 ST7789V3 TFT도 Lab 11/12와 완전히 동일하게 계속 센서 값과 연결 상태를 표시합니다.
- **새로 추가되는 것은 딱 하나, M4의 콘솔 UART(원래 부팅 로그만 찍던 바로 그 UART)를 통해 조작자가 짧은 텍스트 커맨드를 입력하면, M4가 이를 M55에 전달하고 M55가 응답하는 왕복 채널**입니다.

## 왜 새 UART를 추가하지 않고 콘솔 UART를 재사용하는가

이 커리큘럼의 모든 랩은 처음부터 M4 콘솔용으로 USB-to-TTL 컨버터를 J24 헤더에 연결해 왔습니다 (아래 "핀 연결" 절 참고). 이 연결의 13번 핀(M4 TX)과 14번 핀(M4 RX)은 이미 물리적으로 연결되어 있었지만, 지금까지 어떤 랩도 M4의 RX 라인(즉 컨버터에서 M4로 들어오는 입력)을 실제로 사용하지는 않았습니다 — 로그는 항상 M4 → PC 방향(TX)으로만 나갔을 뿐입니다.

이 랩은 그 이미 연결되어 있던 RX 라인을 처음으로 사용합니다. 조작자는 지금까지 부팅 로그를 보던 바로 그 터미널 창에 텍스트를 입력하고 Enter를 누르면 됩니다. 이렇게 하면:

1. 이미 확보된 배선을 그대로 쓰므로 새 하드웨어나 새 핀 연결이 전혀 필요 없습니다.
2. "센서용 I2C 버스와 디스플레이용 SPI 버스에 이어, 이번에는 콘솔 UART까지 — 이 SoC의 핀 하나하나가 이미 여러 기능 후보를 갖고 있다"는 점을 자연스럽게 체감하게 됩니다.

## 핵심 개념

### 1. UART RX 입력 받기 — 인터럽트가 아니라 폴링(polling)으로

이 랩은 처음에 mbox ISR과 동일한 "인터럽트 → `k_msgq` → 워커 스레드" 패턴을 UART RX에도 그대로 적용해보려 했습니다 (`CONFIG_UART_INTERRUPT_DRIVEN=y` + `uart_irq_callback_set()`). 하지만 **실제 하드웨어에서 `uart_irq_callback_set()`이 `-ENOTSUP`(-134)을 반환**했습니다 — 즉 이 보드의 `ns16550_uart0` 인스턴스는 인터럽트 기반 RX 자체를 지원하지 않습니다. Kconfig로 인터럽트 구동 모드를 켰다고 해서 특정 UART 인스턴스의 드라이버가 그 기능을 실제로 구현하고 있다는 보장은 없다는 것을 real-hardware 검증으로 확인한 것입니다. (이 문제를 어떻게 진단했는지는 `13_uart_bridge_troubleshooting_kr.md`에 별도로 정리했습니다.)

그래서 이 랩의 최종 구현은 **폴링 기반**입니다. 전용 스레드(`Uart_Cmd_Task`)가 `uart_poll_in()`을 짧은 주기(10ms)로 반복 호출해서 바이트가 준비되어 있으면 읽고, 없으면 그냥 잠깐 자고 다시 시도합니다.

```c
while (1) {
    if (uart_poll_in(console_dev, &c) != 0) {
        k_msleep(UART_POLL_INTERVAL_MS);   /* 준비된 바이트 없음 -- 잠깐 대기 */
        continue;
    }
    if (c == '\n' || c == '\r') {
        if (line_len == 0) { continue; }
        line_buf[line_len] = '\0';
        line_len = 0;
        /* 아래에서 parse_command() 호출 */
    } else if (line_len < IPC13_LINE_MAX - 1) {
        line_buf[line_len++] = (char)c;
    }
    ...
}
```

인터럽트가 없으므로 이 코드는 처음부터 끝까지 스레드 컨텍스트에서만 실행됩니다 — mbox ISR처럼 "ISR에서는 큐에 넣기만" 하고 별도 스레드로 넘길 필요가 없습니다. 바이트 수집과 커맨드 파싱, M55로의 `mbox_send_dt()` 호출까지 전부 `Uart_Cmd_Task` 한 스레드 안에서 순서대로 이루어집니다. 응답 속도는 폴링 주기(10ms)에 묶이지만, 사람이 타이핑하고 Enter를 누르는 속도에 비하면 충분히 즉각적입니다.

**교훈**: Zephyr 드라이버 API를 쓸 때는 Kconfig 심볼이 컴파일 여부만 결정한다는 것, 그리고 특정 인스턴스가 실제로 그 기능을 구현하는지는 함수의 리턴값으로 확인해야 한다는 것을 기억하세요. 이번처럼 `int` 리턴값을 무시하지 않고 로그로 남겨두면, "왜 반응이 없지?"를 하드웨어 배선 문제와 드라이버 미지원 문제로 빠르게 구분할 수 있습니다.

### 2. 하나의 mbox 채널에 성격이 다른 세 가지 메시지를 함께 싣기

Lab 06은 "커맨드 타입 필드로 여러 종류의 요청/응답을 다중화"하는 구조화된 프로토콜을 처음 소개했습니다. 이 랩은 그 아이디어를 한 단계 더 밀어붙여, **원래 방향도 목적도 다른 두 가지 트래픽 패턴**을 같은 채널에 함께 태웁니다.

```c
enum ipc13_msg_type {
    IPC13_MSG_ENV  = 0,  /* M4 -> M55: 주기적/이벤트성 센서 텔레메트리 (Lab 12와 동일) */
    IPC13_MSG_CMD  = 1,  /* M4 -> M55: 조작자가 입력한 커맨드 전달 */
    IPC13_MSG_RESP = 2,  /* M55 -> M4: 그 커맨드에 대한 응답 */
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

M4는 `tx_channel`로 `IPC13_MSG_ENV`(센서 값)와 `IPC13_MSG_CMD`(조작자 커맨드)를 둘 다 보내고, `rx_channel`로는 `IPC13_MSG_RESP`(M55의 응답)만 받습니다. M55는 반대로 `rx_channel`로 `IPC13_MSG_ENV`와 `IPC13_MSG_CMD`를 둘 다 받아 각각 다른 큐(`env_msgq`, `cmd_msgq`)로 나눠 넣고, `tx_channel`로는 `IPC13_MSG_RESP`만 보냅니다. 이 tx/rx 채널 쌍이 원래부터 양방향으로 쓸 수 있다는 것은 Lab 07(Echo Service)에서 이미 확인된 사실이고, 이 랩은 그 위에 세 번째 메시지 종류를 얹은 것뿐입니다.

한 가지 새로 생기는 문제: M4에서는 이제 **센서 샘플링 루프(main 함수)와 `Uart_Cmd_Task`라는 서로 다른 두 스레드가 같은 `tx_channel`로 동시에 보낼 수 있습니다.** 두 스레드의 `mbox_send_dt()` 호출이 서로 겹치지 않도록, 뮤텍스(`tx_lock`)로 보호합니다.

```c
k_mutex_lock(&tx_lock, K_FOREVER);
mbox_send_dt(&tx_channel, &mbox_msg);
k_mutex_unlock(&tx_lock);
```

### 3. 커맨드 유효성 검사는 최대한 M4에서 먼저

조작자가 오타를 입력했을 때, 이를 M55까지 보내서 "모르는 커맨드"라고 답하게 만들 수도 있지만, 이 랩은 그렇게 하지 않습니다. M4의 `parse_command()`가 알려진 세 가지 커맨드(`1`, `2`, `3 <0|1>`) 중 하나로 해석되지 않으면, M55에는 아무것도 보내지 않고 M4가 그 자리에서 바로 오류를 출력합니다.

> **참고**: 원래는 `PING`/`STATUS`/`MODE <0|1>` 같은 텍스트 명령이었는데, 실습 중 M4 콘솔에 아무 반응이 없는 문제를 좁혀보기 위해 한 자리 숫자(`1`/`2`/`3 <0|1>`)로 바꿨습니다. 터미널 입력·라인엔딩 관련 변수를 줄이고 "커맨드 파싱 문제인지, UART RX 파이프라인 자체 문제인지"를 구분하기 쉽게 하기 위한 것으로, IPC 설계 관점에서 달라지는 것은 없습니다.

```c
if (!parse_command(line_buf, &cmd)) {
    printk("[M4] unknown command: \"%s\" (try: 1=PING, 2=STATUS, 3 <0|1>=MODE, 4 <0|1>=LOG on/off)\n",
           line_buf);
    continue;
}
```

이렇게 하면 M55 쪽 커맨드 처리 코드는 "항상 유효한 값만 들어온다"고 가정하고 단순한 `switch`문으로 짤 수 있고, IPC 왕복 한 번을 아낄 수 있습니다. 잘못된 입력을 어느 쪽에서 걸러낼지도 설계자가 선택할 수 있는 부분이라는 것을 보여주기 위한 의도적인 선택입니다.

### 4. 모든 커맨드가 M55까지 갈 필요는 없다 — 커맨드 `4` (로그 on/off)

실기 테스트 중 `periodic avg ...` 로그가 1초마다 계속 찍혀서 커맨드 입력/응답을 눈으로 확인하기 어렵다는 문제가 있었습니다. 이걸 해결하려고 커맨드 `4 <0|1>`을 추가했는데, 이 커맨드는 `1`/`2`/`3`과 달리 **M55로 아예 전달되지 않습니다.** M4 자신의 콘솔에 찍히는 로그를 끄고 켜는 것뿐이라 M55가 관여할 이유가 없기 때문입니다.

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

`uart_cmd_task_entry()`는 한 줄을 완성하면 `parse_command()`(M55로 보낼 커맨드)를 시도하기 전에 먼저 `handle_local_command()`를 확인합니다 — 로컬 커맨드로 처리됐다면 그걸로 끝, mbox 전송은 아예 일어나지 않습니다. 이건 3번 항목("잘못된 입력은 M4가 먼저 거른다")과는 다른, 한 단계 더 나아간 설계 질문을 보여줍니다: *이 커맨드가 애초에 상대 코어까지 갈 필요가 있는가?* `periodic_log_enabled`는 M4의 `main()` 센서 루프에서 `LOG_INF("periodic avg ...")`를 찍을지 말지만 결정하고, M55로 보내는 센서 텔레메트리(`send_env()`)에는 전혀 영향을 주지 않습니다.

### 5. 커맨드 목록

| 커맨드 | 인자 | 동작 | M55로 전달? |
|---|---|---|---|
| `1` | 없음 | PING — M55가 `"PONG"`으로 응답 | O |
| `2` | 없음 | STATUS — M55가 가장 최근 센서 값 + 연결 상태를 텍스트로 응답 (예: `T:23.5C H:45.2% P:1013.2hPa CONN:OK ERR:0x0`) | O |
| `3 0` / `3 1` | 0 또는 1 | MODE — TFT 화면 맨 아래의 추가 정보 줄을 껐다(0)/켰다(1) 함 | O |
| `4 0` / `4 1` | 0 또는 1 | LOG — M4 콘솔의 `periodic avg ...` 로그를 껐다(0)/켰다(1) 함 | **X (M4-local)** |

`3 1`을 입력하면 M55의 TFT 화면 맨 아래에 `SEQ:<가장 최근 메시지 번호> UP:<M55 부팅 후 경과 초>` 줄이 나타나고, `3 0`을 입력하면 다시 사라집니다. 다만 이 줄은 **다음 센서 텔레메트리 메시지가 도착할 때** 다시 그려지므로, 명령을 입력한 즉시가 아니라 최대 1초 정도 후에 화면에 반영됩니다 — 화면 갱신 로직을 단순하게 유지하기 위한 의도적인 단순화입니다.

`4 0`을 입력하면 그 시점부터 `periodic avg ...` 로그가 더 이상 찍히지 않아 콘솔이 조용해지고, `1`/`2`/`3` 커맨드에 대한 `[M4] forwarded ...` / `[M55] ...` 응답 줄을 방해받지 않고 확인할 수 있습니다. `4 1`로 다시 켤 수 있습니다. 센서 자체는 계속 200ms마다 읽히고 M55로도 계속 전송되므로, TFT 화면 갱신이나 급변 이벤트 감지에는 전혀 영향이 없습니다 — 순전히 M4 콘솔의 "화면 소음"만 조절하는 기능입니다.

### 6. 응답이 다시 M4 화면에 나타나기까지

M55가 `IPC13_MSG_RESP`를 tx_channel로 보내면, M4의 mbox rx 콜백이 이를 받아 `resp_msgq`에 넣고, `Resp_Print_Task`가 그 큐를 기다리다가 꺼내서 화면에 출력합니다.

```c
printk("[M55] %s%s\n", resp.text, resp.ok ? "" : " (rejected)");
```

`LOG_INF()`가 아니라 `printk()`를 쓴 이유는, 다른 `[00:00:00.000,000] <inf> ...` 형식의 로그 줄들 사이에서 조작자에게 온 응답이 한눈에 구분되도록 하기 위해서입니다.

## 아키텍처

```
[조작자 터미널] --텍스트 한 줄--> [M4 콘솔 UART RX]
                                        |
                          uart_poll_in() (Uart_Cmd_Task 스레드, 10ms 주기)
                                        |
                              handle_local_command()?
                                   /              \
                            "4 <0|1>" 이면        그 외 (1/2/3)
                         M4에서 바로 처리            |
                      (M55로 전달 안 함)      parse_command() 통과한 것만
                                                       v
                          mbox tx_channel --IPC13_MSG_CMD-->
                                                                  [M55 rx_cb (ISR)]
                                                                          |
                                                                     cmd_msgq
                                                                          |
                                                                     Cmd_Task (스레드)
                                                                          |  1/2/3 (PING/STATUS/MODE) 처리
                                                                          v
                          <--IPC13_MSG_RESP-- mbox tx_channel <----------+
                                        |
                              M4 rx callback (ISR)
                                        |
                                  resp_msgq
                                        |
                              Resp_Print_Task (스레드)
                                        |
                                        v
                          [조작자 터미널에 "[M55] ..." 로 표시]

(별도로, 지금까지와 동일하게)
[AHT20/BMP280] --200ms마다--> [M4 센서 루프] --IPC13_MSG_ENV--> [M55 Display_Task] --> [TFT]
```

## 핀 연결 (이 랩에서 실제로 사용하는 전체 배선 — Lab 11/12와 동일, 새 배선 없음)

### AHT20 + BMP280 콤보 모듈 (M4, I2C0)

| 모듈 핀 | 연결 대상 | 비고 |
|---|---|---|
| VCC | 3.3V | |
| GND | GND | |
| SDA | SoC GPIO3 (I2C0 SDA) | `i2c0_ms_sda`, 내부 풀업 활성화 |
| SCL | SoC GPIO4 (I2C0 SCL) | `i2c0_ms_scl`, 내부 풀업 활성화 |

### ST7789V3 TFT (M55, SPI0)

| 패널 핀 | 연결 대상 | 비고 |
|---|---|---|
| VCC | 3.3V | |
| GND | GND | |
| SCL/CLK | SPI0 CLK | |
| SDA/MOSI | SPI0 MOSI | |
| CS | SPI0 CS | |
| RES/RST | SoC GPIO17 (J24 3번 핀) | **레벨시프터 필수** — SoC GPIO는 3.3V, 일부 패널의 RST/DC 입력 임계값과 안 맞을 수 있어 레벨시프터를 거쳐 연결 |
| DC | SoC GPIO18 (J24 4번 핀) | 위와 동일하게 레벨시프터 경유 |
| BLK(백라이트) | 3.3V 또는 별도 GPIO | 고정 3.3V 연결도 가능 |

**주의**: SPI0을 켜면 M55의 UART1 콘솔(GPIO23/24 공유)을 꺼야 합니다. 이 랩의 `lab/prj.conf`에 `CONFIG_UART_CONSOLE=n`, `CONFIG_CONSOLE=n`이 이미 설정되어 있습니다. M55 콘솔을 보려면 반드시 **USB-C 직결(J14, 115200bps)** 방법을 사용하세요 — J25 헤더 방식은 이 랩에서 쓸 수 없습니다.

### 조작자 커맨드 입력 (M4 콘솔 UART, 새 배선 없음)

| 대상 | 연결 |
|---|---|
| M4 콘솔 (USB-to-TTL 컨버터) | J24 헤더 13번 핀 = M4 TX, 14번 핀 = M4 RX, GND 공통 (컨버터 TX↔보드 RX, 컨버터 RX↔보드 TX 교차 연결) — 230400bps, 8N1 |

이 연결은 지금까지의 모든 랩에서 M4 부팅 로그를 보기 위해 이미 해 왔던 것과 완전히 동일합니다. 이 랩에서 처음으로, 컨버터를 통해 PC에서 M4로 텍스트를 입력해 보낼 수 있게 됩니다(즉, 이 컨버터가 물린 PC 쪽 터미널 프로그램에서 직접 타이핑).

## devicetree 설정

이 랩의 M4/M55 devicetree 오버레이는 **Lab 12와 완전히 동일**합니다(새 하드웨어가 없으므로 변경할 것이 없습니다). M4 콘솔 UART(`ns16550_uart0`)는 보드 기본 devicetree에서 이미 `zephyr,console`/`zephyr,shell-uart`로 지정되어 있어 별도 오버레이 없이 그대로 사용합니다.

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

`lab/boards/sr100_rdk_sr100_m55.overlay` (SPI0/TFT, I2C1/카메라 비활성화):

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

M55 `CMakeLists.txt`가 `zds,st7789v` 커스텀 바인딩을 찾을 수 있도록 `DTS_ROOT`를 확장하는 것도 Lab 11/12와 동일합니다:

```cmake
list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
```

## prj.conf

`lab/remote/prj.conf` (M4) — Lab 12와 동일:

```
CONFIG_MBOX=y
CONFIG_I2C=y
CONFIG_LOG=y
CONFIG_LOG_DEFAULT_LEVEL=3
CONFIG_PRINTK=y
CONFIG_MAIN_STACK_SIZE=2048
```

커맨드 입력은 `uart_poll_in()`으로 처리하므로(위 "핵심 개념" 1번 참고), 별도의 UART Kconfig 심볼이 필요 없습니다 — 콘솔에 기본으로 켜져 있는 `CONFIG_SERIAL`만으로 충분합니다.

`lab/prj.conf` (M55) — Lab 12와 동일:

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

## 빌드 방법

```bash
# 1) M4 (remote) 이미지 먼저 빌드
west build -p always -b sr100_rdk/sr100/m4 ./zephyr_ipc_sr110/13_uart_bridge/lab/remote -d m4

# 2) M55 (host) 이미지 빌드
west build -p always -b sr100_rdk/sr100/m55 ./zephyr_ipc_sr110/13_uart_bridge/lab -d m55 \
    -DCONFIG_SR100_RELEASE_M4_RESET=y -DM4_BUILD="../m4"
```

## 실행 및 결과 확인

1. M4 콘솔(J24, 230400bps)과 M55 콘솔(J14 USB-C, 115200bps)을 모두 열어둡니다.
2. 보드에 두 이미지를 플래시하고 부팅합니다.
3. M55의 TFT에 "LAB13 BRIDGE" 제목과 함께 연결 상태(WAIT→OK), 센서 상태(SENSOR OK), T/H/P 값이 Lab 12와 동일하게 표시되는지 확인합니다.
4. M4 콘솔 창에 다음을 차례로 입력하고 Enter를 눌러봅니다.
   - `4 0` → `[M4] periodic log OFF (M4-local -- not sent to M55)`가 출력되고, 이후 `periodic avg ...` 로그가 더 이상 찍히지 않는지 확인 (커맨드 응답을 지켜보기 쉬워집니다)
   - `1` → M4 콘솔에 `[M55] PONG`이 출력되는지 확인
   - `2` → `[M55] T:23.5C H:45.2% P:1013.2hPa CONN:OK ERR:0x0` 형태의 응답이 출력되는지 확인 (실제 값은 환경에 따라 다름)
   - `3 1` → `[M55] MODE 1 OK` 응답과 함께, 다음 센서 텔레메트리 갱신 시점(최대 1초 후)에 TFT 화면 맨 아래에 `SEQ:... UP:...s` 줄이 나타나는지 확인
   - `3 0` → 그 줄이 다시 사라지는지 확인
   - `9` (정의되지 않은 커맨드) → M4 콘솔에 `[M4] unknown command: "9" (...)`가 즉시 출력되고, M55로는 아무것도 전송되지 않는지 확인 (M55 로그에 아무 반응이 없어야 정상)
   - `4 1` → `periodic avg ...` 로그가 다시 찍히기 시작하는지 확인

   만약 아무 숫자를 입력해도 M4 콘솔에 `unknown command` 메시지조차 나오지 않는다면, 문제는 커맨드 파싱이 아니라 그 앞단(터미널 → `uart_poll_in()`) 어딘가에 있다는 뜻입니다. 터미널 프로그램의 흐름 제어(flow control) 설정이 `none`인지, 라인엔딩이 CR/LF 중 하나로는 실제로 나가고 있는지 확인하세요.

## 정리

이 랩은 새로운 센서나 디스플레이를 추가하지 않고도, 이미 갖춰진 하드웨어(I2C0의 이중 센서, SPI0의 TFT, 그리고 M4 콘솔 UART) 위에 새로운 IPC 개념 — 하드웨어가 지원하지 않는 기능을 실기 검증으로 걸러내고 폴링으로 대체하기, 하나의 채널에 방향과 목적이 다른 여러 메시지 종류 태워 보내기, 요청/응답 왕복 설계, 그리고 모든 커맨드가 상대 코어까지 갈 필요는 없다는 판단 — 을 얹을 수 있다는 것을 보여줍니다. 다음 랩(Lab 14, Heartbeat Watchdog)도 같은 원칙으로 새 배선 없이 진행됩니다.

**실기 검증 완료**: 커맨드 `1`/`2`/`3 <0|1>`의 M4→M55 왕복, 그리고 `4 <0|1>`의 M4-로컬 처리까지 모두 실제 하드웨어에서 정상 동작이 확인되었습니다.

## 참고

- `MODE` 명령의 화면 반영은 다음 센서 텔레메트리 갱신 시점(최대 1초 후)에 이루어집니다 — 즉시 반영이 필요하다면 `MODE` 응답을 M55가 보낸 직후 강제로 한 번 더 화면을 갱신하는 방식으로 바꿀 수 있습니다.
- 콘솔 UART 인터럽트 미지원 이슈를 실기에서 발견하고 폴링 방식으로 전환한 과정은 `13_uart_bridge_troubleshooting_kr.md`에 별도로 정리했습니다.

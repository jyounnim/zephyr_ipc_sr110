# Lab 06: 구조화된 커맨드 메시지 (Structured Command Message)

M55가 `SET_LED`/`GET_STATUS` 커맨드를 하나의 `struct` 메시지에 담아 2초마다 번갈아 보내고, M4는 커맨드 종류를 판별해 LED를 실제로 켜고 끄거나 현재 상태(LED 상태 + 버튼 누적 횟수)로 응답하는 **커맨드/응답(command-response) 패턴**을 다룹니다.

## 학습 목표

- 값 하나만 실어 나르던 이전 랩들과 달리, **커맨드 타입 필드를 가진 struct**로 여러 종류의 요청을 하나의 IPC 채널에 다중화(multiplexing)하는 방법을 이해한다.
- M55(요청자)가 명령을 보내고 M4(응답자)가 그 결과를 회신하는 **request-response** 흐름을 mbox API 위에서 직접 구현한다.
- 응답이 필요한 커맨드(`GET_STATUS`)와 필요 없는 커맨드(`SET_LED`)를 하나의 프로토콜 안에서 함께 처리하는 구조를 익힌다.
- Lab 03~05에서 확립된 "mbox 콜백은 블로킹 금지, 실제 처리는 워커 스레드에서" 원칙을 좀 더 복잡한 메시지 처리에 적용해본다.

## 이전 랩과의 연결

| Lab | 메시지 내용 | 방향 | 패턴 |
|---|---|---|---|
| 01 | `seq` 하나 | M55 → M4 | 단순 ping |
| 02 | 버튼 이벤트 | M4 → M55 | 단순 알림(notify) |
| 03~05 | 카운터 값 | 양방향 | 값 갱신 + 콜백 논블로킹 |
| **06** | **커맨드 struct(`cmd`, `led_id`, `led_state`) / 상태 struct(`led0_state`, `led1_state`, `button_press_count`)** | **M55 → M4 (커맨드), M4 → M55 (응답)** | **커맨드/응답 다중화** |

앞선 랩들은 메시지가 "카운터 값 하나"처럼 의미가 고정된 단일 데이터였습니다. Lab 06부터는 **메시지 자체가 여러 종류일 수 있다**는 전제 하에, 그 종류를 구분하는 필드(`cmd`)를 두고 하나의 mbox 채널로 다양한 요청을 실어 보내는 방식으로 확장됩니다. 이는 실제 IPC 설계에서 매우 흔한 패턴이며, 이후 랩들에서 다룰 더 복잡한 프로토콜(가변 길이 메시지, 에러 코드 등)의 기초가 됩니다.

## 핵심 IPC 개념: 구조화된 커맨드/응답 프로토콜

### 왜 "값 하나"가 아니라 struct로 커맨드를 설계하는가

지금까지의 랩은 mbox로 넘기는 데이터가 `uint32_t seq` 하나였기 때문에 수신 측이 "무엇을 해야 하는지"를 고민할 필요가 없었습니다. 이 값을 받으면 정해진 동작 하나만 하면 됐습니다. 그러나 실제 시스템에서는 한 코어가 상대 코어에게 요구할 수 있는 동작이 여러 가지입니다(LED를 켜라, 상태를 알려달라, 센서를 읽어라, 리셋해달라 …). 이걸 채널마다 따로 만들면 mbox 채널 수가 기능 수만큼 늘어나 버립니다.

그래서 실무에서는 메시지 맨 앞에 **커맨드 타입 필드**를 두고, 하나의 채널로 여러 종류의 요청을 실어 보내는 방식을 씁니다. 이 랩의 `ipc_command_msg`가 정확히 그 구조입니다.

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

`cmd` 필드가 이 메시지의 "종류"를 나타내고, 나머지 필드(`led_id`, `led_state`)는 커맨드 종류에 따라 의미가 달라지거나 아예 쓰이지 않을 수도 있는 **페이로드(payload)** 입니다. 수신 측(M4)은 `cmd` 값을 보고 나머지 필드를 어떻게 해석할지 분기합니다. 이렇게 하면 채널 하나로 "SET_LED"와 "GET_STATUS"라는 서로 다른 의미의 요청을 동시에 지원할 수 있습니다 — 이것이 **하나의 채널을 여러 논리적 요청 종류로 다중화(multiplexing)한다**는 의미입니다.

### GET_STATUS 같은 커맨드/응답 패턴이 IPC에서 흔한 이유

`SET_LED`는 "지시만 하면 끝"이지만, `GET_STATUS`는 "지금 상태가 어떤지 알아야" 의미가 있습니다. 즉 요청자가 상대방의 **현재 상태를 조회**해야 하는 경우, 요청 하나만으로는 부족하고 반드시 응답이 되돌아와야 합니다. 이것이 커맨드/응답(command-response, request-reply) 패턴이며, IPC에서 다음과 같은 상황마다 반복적으로 등장합니다.

- 한쪽 코어만 알고 있는 하드웨어 상태를 다른 코어가 확인해야 할 때 (이 랩: M4만 LED GPIO와 버튼 카운트를 갖고 있음)
- 명령이 실제로 적용됐는지 성공/실패를 확인해야 할 때
- 두 코어의 상태를 동기화해야 할 때

이 랩에서 M55는 **요청자(requester)**, M4는 **응답자(responder)** 역할을 명확히 나눠 맡습니다. M55는 상태를 직접 알 수 없으므로(LED와 버튼은 물리적으로 M4에만 연결) `GET_STATUS`를 보내고 그 결과를 수동적으로 기다립니다. M4는 자신이 갖고 있는 실제 상태(`g_led0_state`, `g_led1_state`, `g_button_presses`)를 그대로 담아 `struct ipc_status_msg`로 회신합니다.

```c
/* M4 -> M55 reply (sent only in response to GET_STATUS) */
struct ipc_status_msg {
	uint32_t led0_state;
	uint32_t led1_state;
	uint32_t button_press_count;
};
```

주의할 점은, 이 랩의 mbox는 "요청에 대한 응답"이라는 상관관계를 프로토콜 차원에서 보장해주지 않는다는 것입니다. M55는 `GET_STATUS`를 보낸 뒤 그냥 자신의 rx 콜백에 다음으로 들어오는 메시지를 상태 응답이라고 "가정"합니다. `SET_LED`에는 응답이 없기 때문에 이 가정이 성립하지만, 만약 M4가 매 커맨드마다 확인 응답(ack)까지 보내는 프로토콜이었다면 M55는 "이 응답이 어느 요청에 대한 것인지" 구분할 별도의 요청 ID 같은 필드가 필요했을 것입니다. 지금 구조는 교육 목적상 가장 단순한 형태이며, 실제 제품에서는 이런 요청-응답 상관관계 처리가 프로토콜 설계의 핵심 포인트 중 하나입니다.

### 메시지 struct 설계 시 주의점

이 랩의 두 struct는 모두 `uint32_t` 필드로만 구성되어 있습니다. 이는 우연이 아니라 의도적인 설계입니다.

- **크기 고정**: mbox는 바이트 버퍼를 그대로 복사해서 전달하는 저수준 API입니다(`mbox_msg.data`, `mbox_msg.size`). 송신 측과 수신 측이 서로 다른 struct 레이아웃을 갖고 있으면 `memcpy`로 엉뚱한 값이 채워집니다. 그래서 `ipc_command_msg`/`ipc_status_msg`를 `ipc_common.h` 하나에 정의해 M55/M4 양쪽 빌드가 **동일한 정의를 include** 하도록 강제합니다.
- **정렬/패딩**: `uint32_t` 필드만 나란히 두면 컴파일러가 필드 사이에 패딩 바이트를 끼워 넣을 여지가 없습니다(모든 필드가 4바이트 정렬이라 자연 정렬됨). 만약 `uint8_t`와 `uint32_t`를 섞어서 정의했다면, M4와 M55가 서로 다른 컴파일러 옵션이나 구조체 packing 설정을 쓸 경우 패딩 위치가 달라질 위험이 있습니다. 이 랩에서는 필드 타입을 통일해 그런 위험을 원천 차단했습니다.
- **수신 측 크기 검증**: 두 `mbox_rx_callback()` 모두 `memcpy` 전에 `if (data->size < sizeof(...))`로 먼저 검사합니다. 이는 상대방이 다른 크기의 메시지를 보냈거나(예: 다른 랩의 이미지가 잘못 플래시된 경우), IPC 채널에 문제가 생겨 크기가 깨진 경우에도 버퍼 오버리드 없이 안전하게 무시하기 위함입니다.

## 아키텍처 / 코드 설명

### M55 (HOST, `lab/src/main.c`) — 요청자

`main()`은 5단계 상태(`step % 5`)를 순환하며 2초마다 커맨드를 하나씩 보냅니다: `SET_LED led0=ON` → `SET_LED led1=ON` → `GET_STATUS` → `SET_LED led0=OFF` → `SET_LED led1=OFF` → (반복). `send_cmd()`는 `struct ipc_command_msg`를 그대로 `mbox_send_dt()`에 넘길 뿐이며, 별도의 스레드 분리 없이 `main()` 루프에서 직접 전송합니다. 수신 콜백(`mbox_rx_callback`)은 M4가 회신한 `struct ipc_status_msg`를 크기 검증 후 로그로 출력만 하며, 블로킹 호출이 없어 mbox ISR 컨텍스트에서 안전합니다.

### M4 (CLIENT, `lab/remote/src/main.c`) — 응답자

이 랩은 Lab 03에서 확립된 "콜백은 큐에 넣고 즉시 반환, 실제 처리는 워커 스레드" 구조를 그대로 따릅니다.

1. `mbox_rx_callback()`은 수신한 바이트를 `struct ipc_command_msg`로 `memcpy`한 뒤 `k_msgq_put()`으로 `cmd_msgq`에 넣고 즉시 반환합니다. (ISR 컨텍스트, 블로킹 금지)
2. `Cmd_Task` 워커 스레드(`cmd_task_entry`)가 `k_msgq_get()`으로 커맨드를 하나씩 꺼내 `cmd.cmd` 값에 따라 분기합니다.
   - `IPC_CMD_SET_LED`: `cmd.led_id`로 LED0/LED1을 선택해 `gpio_pin_set_dt()`로 실제 GPIO를 제어하고, 전역 상태(`g_led0_state`/`g_led1_state`)도 함께 갱신합니다.
   - `IPC_CMD_GET_STATUS`: 전역 상태(`g_led0_state`, `g_led1_state`, `g_button_presses`)를 `struct ipc_status_msg`에 채워 `mbox_send_dt()`로 M55에 회신합니다.
   - 그 외 알 수 없는 `cmd` 값은 `LOG_WRN()`으로 경고만 남기고 무시합니다 (프로토콜이 확장돼도 구버전 펌웨어가 죽지 않도록 하는 최소한의 방어).
3. 버튼(`user_button`) 입력은 Zephyr Input 서브시스템 콜백(`button_input_cb`)에서 `g_button_presses`를 누적 증가시킵니다. 이 값은 다음 `GET_STATUS` 응답에 실려 M55로 전달됩니다.

## devicetree 설정 설명

이 랩의 오버레이는 앞선 랩들에서 이미 확정된 두 가지 보드 이슈를 그대로 이어받습니다.

- **`&i2c1` M55 측 disable** (`lab/boards/sr100_rdk_sr100_m55.overlay`): M4와 M55가 물리적으로 같은 I2C1 버스(LED0/LED1/버튼이 달린 `gpio_exp0`, PCA6416A)를 공유하기 때문에, LED/버튼을 실제로 구동하는 M4만 이 버스를 갖도록 M55 쪽 `&i2c1`과 그 자식 노드(`&gpio_exp0`, `&ov02c10`)를 `disabled` 처리합니다. 원인은 [Lab 01 트러블슈팅 문서](../../01_hello_ipc/doc/01_hello_ipc_troubleshooting_kr.md) 참고.
- **`&buttons { polling-mode; };`** (`lab/remote/boards/sr100_rdk_sr100_m4.overlay`): M4 쪽 `gpio_exp0`에는 `int-gpios`가 배선돼 있지 않아 인터럽트 기반 버튼 감지가 `-ENOTSUP(-134)`로 실패합니다. 부모 노드인 `&buttons`(자식 `&user_button`이 아님)에 `polling-mode`를 지정해 주기적 폴링(기본 debounce 30ms)으로 전환합니다. 원인은 [Lab 02 트러블슈팅 문서](../../02_button_pong/doc/02_button_pong_troubleshooting_kr.md) 참고.
- **`ipc0` shared-memory-size**: M4/M55 양쪽 오버레이 모두 `shared-memory-size = <0x400>;`로 동일하게 맞춰야 합니다. 두 값이 다르면 mbox 채널이 참조하는 공유 메모리 레이아웃이 어긋나 빌드 또는 런타임 오류로 이어질 수 있습니다.

## 빌드 방법

```bash
# 1) M4(remote) 먼저 빌드
west build -p always -b sr100_rdk/sr100/m4 ./zephyr_ipc_sr110/06_structured_command/lab/remote -d m4

# 2) M55(host, M4 바이너리를 M4_BUILD로 포함)
west build -p always -b sr100_rdk/sr100/m55 ./zephyr_ipc_sr110/06_structured_command/lab -d m55 \
    -DCONFIG_SR100_RELEASE_M4_RESET=y -DM4_BUILD="../m4"
```

> **주의**: `M4_BUILD`는 M55 빌드 디렉토리(`m55/`) 기준 상대경로입니다. `m4/`와 `m55/`가 워크스페이스 루트의 형제 디렉토리라면 `../m4`가 맞습니다 (자세한 원인은 [Lab 01 트러블슈팅 문서](../../01_hello_ipc/doc/01_hello_ipc_troubleshooting_kr.md) 참고).

M4를 먼저 빌드해야 하는 이유는 M55 빌드 시 `M4_BUILD` 경로로 M4의 ELF 산출물을 참조해 최종 이미지에 함께 포함시키기 때문입니다.

## 실행 및 결과 확인

각 코어의 시리얼 콘솔을 230400bps 8N1로 열어서 확인합니다.

- **M55 콘솔**: `CMD -> SET_LED led0=ON` 등 2초마다 커맨드 로그가 순환 출력되고, `GET_STATUS` 차례에는 M4의 응답이 `STATUS <- led0=N led1=N button_presses=N` 형태로 출력됩니다.
- **M4 콘솔**: `SET_LED led_id=N state=N applied`, `GET_STATUS replied` 로그가 출력됩니다. 실제 보드의 LED0/LED1이 커맨드에 따라 켜지고 꺼지는 것을 눈으로 확인할 수 있습니다. `user_button`을 누르면 다음 `GET_STATUS` 응답의 `button_presses`에 반영됩니다(폴링 모드이므로 debounce-interval-ms(기본 30ms) 이후 반영됨).

## 정리 / 핵심 요약

- 메시지 앞부분에 **커맨드 타입 필드**를 두면, 하나의 mbox 채널로 여러 종류의 요청을 다중화할 수 있다.
- **커맨드/응답 패턴**은 요청자가 상대방만 아는 상태를 조회해야 할 때 반드시 필요하며, 이 랩에서는 M55(요청자)/M4(응답자)로 역할이 명확히 나뉜다.
- 메시지 struct는 필드 타입을 통일해 **패딩/정렬 문제를 예방**하고, 양쪽 빌드가 헤더 하나(`ipc_common.h`)를 공유해 레이아웃 불일치를 막는다.
- 수신 콜백은 여전히 **크기 검증 후 큐에 넣고 즉시 반환**해야 하며, 실제 커맨드 처리는 워커 스레드에서 수행한다(Lab 03 원칙의 연장).

## 다음 랩 예고

이 랩까지는 M55가 커맨드를 보내면 M4가 응답할 때까지 그냥 기다리지 않고 **다음 커맨드로 넘어가는 비동기(fire-and-forget에 가까운) 방식**이었습니다. Lab 07(Echo Service)에서는 M55가 보낸 데이터를 M4가 그대로 되돌려 보내는 왕복(echo) 구조로 IPC 신뢰성을 검증하면서, 이 커리큘럼에서 가장 중요한 설계 원칙 하나가 실기 테스트로 확정됩니다 — **`mbox_send_dt()`조차 mbox 콜백(ISR 컨텍스트) 안에서 직접 호출하면 안 된다**는 것입니다. 지금까지는 "콜백은 큐에 넣기만 하고, 무거운 처리만 워커 스레드로 미룬다"는 원칙이었다면, Lab 07부터는 "메시지를 되돌려 보내는 `mbox_send_dt()` 호출 자체도 예외 없이 워커 스레드에서만" 이루어져야 한다는 점을 실제 hang 사례와 함께 배우게 됩니다.

---

문제가 발생했다면 → [`06_structured_command_troubleshooting_kr.md`](./06_structured_command_troubleshooting_kr.md) 참고

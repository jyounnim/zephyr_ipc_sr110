# Lab 04: Shared Counter (M55 → M4)

M55가 1초마다 증가시키는 카운터 값을 M4에 전달하고, M4는 그 값을 LED 점멸 횟수로 "표시"하는 랩입니다. Lab 01~03에서 익힌 mbox 신호 전송에 이어, 이번 랩부터는 **실제 데이터(payload)를 담은 메시지**를 주고받습니다.

## 학습 목표

- mbox 메시지에 구조체(struct) payload를 실어 보내는 방법을 익힌다.
- "공유 상태(shared state)"를 코어 간에 일치시키는 것이, 진짜 메모리를 공유하는 것과 어떻게 다른지 이해한다.
- 수신한 데이터를 ISR 콜백이 아니라 별도 워커 스레드에서 처리해야 하는 이유를, 실제로 블로킹 동작(LED 점멸)이 필요한 코드로 확인한다.
- devicetree의 `shared-memory-size`가 mbox 메시지 크기와 어떤 관계인지 이해한다.

## 이전 랩과의 연결

Lab 01~03은 mbox로 "신호"만 주고받았습니다 — payload 크기가 0이거나 아주 작아서, 메시지 자체에 의미 있는 데이터를 담지 않고 "이벤트가 발생했다"는 사실만 전달했습니다(M55→M4 ping, M4→M55 pong 등).

이번 Lab 04부터는 처음으로 **의미 있는 데이터**(`uint32_t counter` 값)를 mbox 메시지에 실어 보냅니다. M55는 1초마다 카운터를 1씩 증가시켜 M4로 전송하고, M4는 받은 값을 `counter % 5 + 1`번 LED0를 깜빡여 화면 없이도 값을 눈으로 확인할 수 있게 "표시"합니다.

또한 Lab 03에서 확립된 원칙 — **mbox 수신 콜백(ISR 컨텍스트)은 절대 블로킹되면 안 된다** — 을 이번 랩에서 처음으로 "블로킹이 실제로 필요한 처리"(LED를 여러 번 켰다 껐다 하려면 `k_msleep()`으로 시간 간격을 둬야 함)를 다루면서 왜 이 원칙이 중요한지 체감하게 됩니다.

## 핵심 IPC 개념: "공유 상태"이지 "공유 메모리"가 아니다

랩 제목이 "Shared Counter"이다 보니 오해하기 쉬운 부분이 있습니다: **M55와 M4가 counter라는 변수를 물리적으로 공유하고 있는 것이 아닙니다.**

### 메시지 패싱 vs 진짜 공유 메모리

두 가지 접근 방식을 구분해서 이해할 필요가 있습니다.

1. **진짜 공유 메모리(shared memory) 방식**: 두 코어가 같은 물리 주소를 매핑해서, 한쪽이 그 주소에 값을 쓰면 다른 쪽이 별도의 통지 없이도 (혹은 폴링으로) 그 값을 읽어갈 수 있는 구조. 이 경우 두 코어가 동시에 같은 변수를 건드릴 수 있으므로 뮤텍스나 아토믹 연산 같은 동기화 프리미티브가 필수입니다.
2. **메시지 패싱(message passing) 방식** — 이 랩이 실제로 쓰는 방식: M55는 자신의 로컬 변수 `msg.counter`를 증가시킨 뒤, 그 값을 **통째로 복사한 메시지**를 mbox를 통해 M4에 "보냅니다". M4는 그 메시지를 받아서 **자신의 로컬 복사본**(`struct ipc_counter_msg msg`, `blink_task_entry()`의 스택 변수)에 저장해 사용합니다.

즉 "counter가 공유되어 있다"는 것은 **논리적인 의미**일 뿐입니다 — 두 코어가 같은 값을 "알고 있다"는 뜻이지, 같은 메모리 주소를 참조하고 있다는 뜻이 아닙니다. 실제로는 매 초마다 M55가 자신의 카운터 값을 스냅샷으로 찍어 M4에 우편으로 보내는 것에 가깝습니다. M4가 이 값을 받아 처리하는 동안 M55가 이미 다음 값으로 카운터를 증가시켰더라도, M4가 들고 있는 복사본에는 영향이 없습니다.

devicetree의 `&ipc0 { shared-memory-size = <0x400>; };`가 "공유 메모리"라는 이름을 쓰고 있지만, 이것은 **애플리케이션이 직접 읽고 쓰는 변수 공간이 아니라 mbox 드라이버가 메시지를 코어 간에 실어 나르는 데 내부적으로 쓰는 전송 버퍼(MTU)**입니다. `mbox_send_dt()`/수신 콜백이라는 API 경계 뒤에 완전히 캡슐화되어 있어서, 애플리케이션 코드 입장에서는 그 존재를 신경 쓸 필요가 없습니다 — 그냥 "메시지를 보낸다/받는다"는 함수 호출만 있을 뿐입니다.

### 그래서 이 랩에는 뮤텍스가 없다

이렇게 메시지 패싱 방식을 쓰면, **동시에 같은 메모리 위치를 두 코어가 건드릴 일 자체가 없으므로** race condition을 피하기 위한 뮤텍스나 세마포어 같은 코어 간 동기화 프리미티브가 필요하지 않습니다. `counter` 값은 항상 "M55의 로컬 변수" 아니면 "M4가 방금 받은 로컬 복사본" 둘 중 하나로만 존재하고, 그 경계를 넘나드는 순간은 오직 mbox 메시지 전송이라는 단일 지점뿐입니다.

다만 이 랩에는 **코어 간** 동기화가 아니라 **M4 내부의 한 코어 안에서** 스레드 간 동기화를 위한 자료구조가 하나 등장합니다: `K_MSGQ_DEFINE(rx_msgq, ...)`. 이건 뒤에서 설명합니다.

## 아키텍처 및 코드 설명

### 메시지 정의 (`lab/include/ipc_common.h`)

```c
struct ipc_counter_msg {
	uint32_t counter;
};
```

이전 랩들보다 나아간 부분은 여기입니다 — payload가 있는 구조체를 정의하고, 이 구조체 전체를 mbox 메시지로 주고받습니다.

### M55 (host) — `lab/src/main.c`

```c
struct mbox_dt_spec tx_channel;
struct ipc_counter_msg msg = {.counter = 0};
struct mbox_msg mbox_msg;

tx_channel = (struct mbox_dt_spec)MBOX_DT_SPEC_GET(DT_PATH(mbox_consumer), tx);

while (1) {
	msg.counter++;

	mbox_msg.data = &msg;
	mbox_msg.size = sizeof(msg);
	ret = mbox_send_dt(&tx_channel, &mbox_msg);
	...
	k_msleep(TICK_PERIOD_MS);
}
```

M55는 mbox 수신 콜백이 아예 없는 **단방향(M55 → M4) 랩**입니다. `main()`의 while 루프 자체가 이미 스레드 컨텍스트이므로, `k_msleep(1000)`으로 1초 대기하고 `mbox_send_dt()`를 호출해도 아무 문제가 없습니다. Lab 03의 "콜백은 블로킹 금지" 원칙은 애초에 콜백이 없는 이 코드에는 해당되지 않습니다.

`mbox_msg.data = &msg; mbox_msg.size = sizeof(msg);`가 이번 랩의 핵심입니다 — payload의 시작 주소와 크기를 지정해서 `mbox_send_dt()`에 넘기면, 드라이버가 그 내용을 M4 쪽 수신 버퍼로 옮겨줍니다.

### M4 (client) — `lab/remote/src/main.c`

M4 쪽은 **ISR 콜백 → 메시지 큐(`k_msgq`) → 워커 스레드**라는 3단 구조로 되어 있습니다.

```c
K_MSGQ_DEFINE(rx_msgq, sizeof(struct ipc_counter_msg), 4, 4);

static void mbox_rx_callback(const struct device *dev, mbox_channel_id_t channel_id,
			      void *user_data, struct mbox_msg *data)
{
	struct ipc_counter_msg msg;

	if (data->size < sizeof(msg)) {
		return;
	}
	memcpy(&msg, data->data, sizeof(msg));
	k_msgq_put(&rx_msgq, &msg, K_NO_WAIT);
}
```

`mbox_rx_callback()`은 **ISR 컨텍스트**에서 실행됩니다. 여기서 하는 일은 딱 두 가지뿐입니다: 수신 데이터를 로컬 변수로 복사하고, 그 복사본을 메시지 큐에 넣는 것(`K_NO_WAIT`이므로 큐가 가득 차 있어도 블로킹하지 않고 즉시 반환). LED를 몇 번 깜빡일지 계산하거나 실제로 GPIO를 토글하는 작업은 **전혀 하지 않습니다.**

```c
static void blink_task_entry(void *p1, void *p2, void *p3)
{
	struct ipc_counter_msg msg;
	uint32_t blink_count;

	while (1) {
		if (k_msgq_get(&rx_msgq, &msg, K_FOREVER) != 0) {
			continue;
		}

		blink_count = (msg.counter % 5U) + 1U;
		LOG_INF("counter=%u received -> blinking LED0 %u times", msg.counter, blink_count);

		for (uint32_t i = 0; i < blink_count; i++) {
			gpio_pin_set_dt(&led0, 1);
			k_msleep(BLINK_ON_MS);
			gpio_pin_set_dt(&led0, 0);
			k_msleep(BLINK_OFF_MS);
		}
	}
}
```

실제 값 해석(`counter % 5 + 1`)과 LED 점멸(`k_msleep()`을 반복 호출하는 블로킹 동작)은 별도로 생성한 `Blink_Task`라는 워커 스레드에서 처리됩니다. 이 스레드는 `k_msgq_get(&rx_msgq, &msg, K_FOREVER)`로 큐에 새 메시지가 들어올 때까지 잠들어 있다가, 메시지가 들어오면 깨어나 처리합니다.

**왜 이렇게 나눠야 하는가**: LED를 `blink_count`번 깜빡이려면 켜고(150ms) 끄고(150ms)를 반복해야 하고, 이 "150ms 동안 기다리기"는 명백한 블로킹 동작입니다. 만약 이 로직을 ISR 컨텍스트인 `mbox_rx_callback()` 안에 그대로 넣었다면, 인터럽트 핸들러가 최대 1.5초(5회 × 300ms) 가까이 커널을 점유하게 되어 다른 인터럽트 처리가 막히고 시스템이 사실상 멈춘 것처럼 동작했을 것입니다. Lab 03에서 mbox 콜백 안에 `k_msleep()`을 직접 넣었다가 커널이 완전히 멈춰버린 것과 같은 종류의 버그입니다. `k_msgq`를 매개로 "수신"과 "처리"를 서로 다른 실행 컨텍스트로 분리하는 것이 이 문제를 근본적으로 피하는 표준 패턴입니다.

## devicetree 설정 설명

### `lab/boards/sr100_rdk_sr100_m55.overlay` (M55)

```dts
&ipc0 {
	shared-memory-size = <0x400>;
};

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

- `shared-memory-size = <0x400>;`: mbox 메시지 전송에 쓰이는 내부 전송 버퍼(MTU) 크기입니다. M4/M55 양쪽 오버레이에 반드시 **동일한 값**을 지정해야 합니다. 이 랩에서 실제로 보내는 메시지(`struct ipc_counter_msg`, 4바이트)는 이 한도에 비하면 매우 작습니다.
- `&i2c1`, `&gpio_exp0`, `&ov02c10`을 `disabled`로 두는 것은 이 랩만의 설정이 아니라 **전체 랩 공통 설정**입니다. M4와 M55는 물리적으로 같은 I2C1 버스(LED0/LED1/버튼이 매달린 GPIO expander `gpio_exp0`가 이 버스에 있음)를 공유하는데, 두 코어가 동시에 이 버스를 초기화하려 들면 충돌이 나므로 M55 쪽에서 이 버스와 그 하위 장치들을 명시적으로 꺼서 M4만 LED/버튼을 소유하도록 정리한 것입니다.

### `lab/remote/boards/sr100_rdk_sr100_m4.overlay` (M4)

```dts
&ipc0 {
	shared-memory-size = <0x400>;
};
```

M4 쪽은 `shared-memory-size` 값만 M55와 맞춰주면 됩니다. LED0는 M4의 base devicetree에서 기본으로 활성화되어 있으므로 별도 오버레이가 필요 없습니다.

## 빌드 방법

```bash
# 1) M4(remote) 먼저 빌드
west build -p always -b sr100_rdk/sr100/m4 ./zephyr_ipc_sr110/04_shared_counter/lab/remote -d m4

# 2) M55(host, M4 바이너리를 M4_BUILD로 포함)
west build -p always -b sr100_rdk/sr100/m55 ./zephyr_ipc_sr110/04_shared_counter/lab -d m55 \
    -DCONFIG_SR100_RELEASE_M4_RESET=y -DM4_BUILD="../m4"
```

> **주의**: `M4_BUILD`는 M55 빌드 디렉토리(`m55/`) 기준 상대경로입니다. `m4/`와 `m55/`가 워크스페이스 루트의 형제 디렉토리라면 `../m4`가 맞습니다.

## 실행 및 결과 확인

정상 동작 시 두 코어의 콘솔 출력은 다음과 같습니다.

- **M55 콘솔**: `counter=N sent`가 매초 출력됩니다(N은 1부터 계속 증가).
- **M4 콘솔**: `counter=N received -> blinking LED0 M times`가 출력되며, 실제로 LED0가 `M = N % 5 + 1`번 짧게 깜빡입니다. 즉 N=1일 때 2회, N=5일 때 1회, N=6일 때 2회… 식으로 반복됩니다.

## 정리

- Lab 04에서 처음으로 mbox 메시지에 **의미 있는 데이터**(struct payload)를 실어 보냈습니다.
- "공유 상태"는 메시지 패싱을 통해 M55와 M4가 같은 값을 논리적으로 공유한다는 뜻이며, 물리적으로 같은 메모리 변수를 공유하는 것과는 다릅니다 — 그래서 코어 간 동기화 프리미티브(뮤텍스 등)가 필요 없습니다.
- devicetree의 `shared-memory-size`는 애플리케이션이 직접 다루는 자원이 아니라 mbox 드라이버 내부의 전송 버퍼(MTU)입니다.
- M4 쪽은 ISR 콜백(`mbox_rx_callback`) → 메시지 큐(`rx_msgq`) → 워커 스레드(`Blink_Task`)로 이어지는 구조를 통해, 블로킹이 필요한 실제 처리(LED 점멸)를 ISR 컨텍스트 밖에서 안전하게 수행합니다.

## 다음 랩 예고

Lab 05에서는 이번과 반대 방향으로 — M4가 버튼을 누른 횟수를 세어 M55에 전달하는 구조를 다룹니다. 같은 "카운터를 메시지로 전달"하는 패턴을 M4 → M55 방향으로 뒤집어 연습합니다.

---

문제가 발생했다면 → [`04_shared_counter_troubleshooting_kr.md`](./04_shared_counter_troubleshooting_kr.md) 참고

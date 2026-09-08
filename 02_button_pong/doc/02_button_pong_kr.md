# Lab 02: Button Pong — M4가 감지한 버튼 입력을 M55로 전달하기

M4가 실제 GPIO(버튼) 인터럽트를 처리해 얻은 "이벤트"를 mbox IPC로 M55에 전달하는 랩입니다. Lab 01의 단순 핑퐁을 넘어, 실제 임베디드 시스템에서 가장 흔한 패턴인 **"디바이스를 소유한 코어가 이벤트를 감지하고, 다른 코어에 통지한다"**를 다룹니다.

## 학습 목표

이 랩을 마치면 다음을 할 수 있습니다.

- Zephyr **Input 서브시스템**(`zephyr/input/input.h`)을 이용해 `gpio-keys` 기반 버튼 입력을 콜백으로 받는 코드를 작성할 수 있다.
- 인터럽트/콜백 컨텍스트에서 발생한 이벤트를 **메시지 큐(`k_msgq`) + 워커 스레드** 패턴으로 안전하게 처리할 수 있다.
- 한 코어(M4)만 소유한 주변장치(I2C GPIO expander)의 이벤트를 mbox로 다른 코어(M55)에 전달하는 구조를 설계할 수 있다.
- devicetree에서 `polling-mode` 같은 드라이버별 우회 옵션이 왜, 언제 필요한지 판단할 수 있다.
- mbox 콜백이 ISR 컨텍스트에서 실행될 때, 어떤 작업을 콜백 안에서 바로 처리해도 되고 어떤 작업은 워커 스레드로 넘겨야 하는지 구분할 수 있다.

## 이전 랩과의 연결

Lab 01(`Hello IPC`)에서는 M55가 먼저 "ping"을 보내고 M4가 "pong"으로 응답하는, 소프트웨어적으로 발생시킨 순수 IPC 핑퐁을 다뤘습니다. 메시지를 보내는 트리거가 코드 안에 있었고(타이머 또는 루프), 실제 하드웨어 이벤트는 관여하지 않았습니다.

Lab 02는 여기서 두 가지를 확장합니다.

1. **트리거가 소프트웨어가 아니라 실제 하드웨어 인터럽트(버튼)** 라는 점. 이제 IPC 메시지는 "언제 보낼지 정해진 타이밍"이 아니라 "언제 일어날지 알 수 없는 비동기 이벤트"에 의해 발생합니다.
2. **방향이 M4 → M55로 고정**된다는 점. 이 보드에서 버튼(SW8)과 LED는 모두 I2C GPIO expander(`gpio_exp0`, PCAL6416A) 뒤에 달려 있고, M4/M55가 물리적으로 같은 I2C1 버스를 공유하는 제약 때문에 이 랩을 포함한 모든 랩에서 `gpio_exp0`은 M4만 소유합니다(자세한 배경은 Lab 01 문서의 I2C1 버스 공유 설명 참고). 즉 버튼도 LED도 M4만 만질 수 있고, M55는 "무슨 일이 일어났는지"를 통보받는 관찰자 역할만 합니다.

정리하면, Lab 01이 "IPC 채널이 살아있는지"를 확인하는 랩이었다면, Lab 02는 "실제 디바이스 이벤트를 다른 코어로 전달하는" 더 현실적인 시나리오입니다.

## 핵심 IPC 개념

### 1. M4 → M55: "디바이스 이벤트를 호스트로 전달하는" 패턴

이 랩의 아키텍처는 흔히 **센서/주변장치 코어(M4)가 이벤트를 감지하고, 애플리케이션 코어(M55)가 그 이벤트를 소비한다**는 비대칭 구조를 보여줍니다.

```
        [ SW8 버튼 ]
              │  (I2C, gpio_exp0)
              ▼
      ┌───────────────┐
      │   M4 (CLIENT) │  버튼 입력 감지 + LED1 로컬 토글
      │               │
      │  Input 콜백    │
      │      │        │
      │      ▼        │
      │  k_msgq       │
      │      │        │
      │      ▼        │
      │ Button_Task   │──── mbox_send_dt() ────┐
      └───────────────┘                        │
                                                ▼
                                        ┌───────────────┐
                                        │  M55 (HOST)   │
                                        │  mbox 콜백에서 │
                                        │  로그만 출력   │
                                        └───────────────┘
```

M4는 이 보드에서 `gpio_exp0`(LED0/LED1/버튼)을 소유한 유일한 코어이므로, 버튼을 누르면 M4가 먼저 반응합니다(LED1 토글로 즉각적인 시각 피드백). 그 다음 mbox로 "몇 번째 이벤트인지"만 담은 최소한의 메시지를 M55에 보냅니다. M55는 이 메시지를 받아 콘솔에 로그를 남기는 것 외에는 아무 것도 하지 않는데, 이는 **"M55가 LED1을 직접 켤 수 없기 때문"**입니다 — LED1도 같은 `gpio_exp0` 뒤에 있고, M55 쪽에서는 I2C1이 disable되어 있어 물리적으로 접근할 수 없습니다.

이 구조는 실무에서 매우 흔합니다. 예를 들어 M4가 센서나 버튼 같은 저수준 주변장치를 담당하고, M55(애플리케이션 프로세서)는 그 이벤트를 받아 UI 갱신, 로깅, 상위 로직 처리 등을 담당하는 식입니다. "누가 하드웨어를 소유하는가"가 IPC 메시지의 방향과 페이로드 설계를 결정한다는 점을 기억해 두세요.

### 2. GPIO 인터럽트 콜백과 mbox 콜백이 함께 동작할 때의 ISR 안전성

이 랩의 M4 쪽 코드에는 인터럽트/콜백 컨텍스트에서 실행되는 코드가 두 겹으로 존재합니다.

- Zephyr Input 서브시스템의 `INPUT_CALLBACK_DEFINE()`으로 등록한 `button_input_cb()`는 **인터럽트에 가까운 컨텍스트**(정확히는 입력 드라이버의 워크큐 또는 ISR — 드라이버에 따라 다름)에서 호출됩니다.
- (Lab 01에서 이미 다룬) mbox의 수신 콜백 역시 **ISR 컨텍스트**에서 실행됩니다.

두 경우 모두 공통된 제약이 있습니다. **ISR/콜백 컨텍스트에서는 블로킹 가능한 작업(I2C 트랜잭션, `k_sleep`, 뮤텍스 대기 등)을 직접 수행하면 안 됩니다.** `button_input_cb()`가 곧바로 `gpio_pin_toggle_dt(&led1)`(I2C 버스 트랜잭션)와 `mbox_send_dt()`를 호출한다면, 콜백을 호출한 드라이버의 컨텍스트에 따라 커널이 assert를 발생시키거나 최소한 시스템 전체의 응답성을 해칠 수 있습니다.

그래서 `button_input_cb()`는 실제 작업을 전혀 하지 않고, 오직 "눌림 이벤트가 있었다"는 사실 하나만 `k_msgq_put()`으로 큐에 넣고 즉시 리턴합니다(`K_NO_WAIT` — 큐가 가득 차 있어도 콜백을 블로킹하지 않음). 실제 LED 토글과 mbox 전송은 별도의 `Button_Task` 워커 스레드가 큐에서 이벤트를 꺼내 스레드 컨텍스트에서 처리합니다. 이는 Lab 01에서 이미 등장한 "ISR/콜백 → `k_msgq` → 워커 스레드" 패턴을 그대로 재사용한 것입니다 — 콜백이 두 겹(입력 콜백 + 향후 mbox 콜백)이 되어도 동일한 패턴 하나로 둘 다 안전하게 처리할 수 있음을 보여줍니다.

반대로 M55 쪽 `mbox_rx_callback()`은 콜백 안에서 바로 `LOG_INF()`만 호출하고 끝냅니다. 로그 출력은 (설정에 따라 다르지만 일반적으로) 블로킹 가능성이 낮고 짧게 끝나는 작업이라, 별도의 워커 스레드로 넘기지 않고 콜백 안에서 직접 처리해도 괜찮다는 것을 보여주는 대조 사례입니다. **"콜백이 ISR/근접 ISR 컨텍스트라고 해서 무조건 워커 스레드로 넘겨야 하는 것은 아니고, 콜백 안에서 수행할 작업이 블로킹 가능성이 있는지(I2C, 뮤텍스, 긴 연산 등)가 판단 기준**이라는 점을 M4 쪽과 M55 쪽 코드가 각각 보여줍니다.

### 3. `polling-mode`가 필요한 이유 — gpio_exp0의 하드웨어적 제약

M4를 처음 빌드해서 실행하면 부팅 로그에 다음과 같은 에러가 나올 수 있습니다(이미 최종 코드에는 반영되어 있으므로 지금 빌드하면 나타나지 않습니다).

```
<err> gpio_keys: interrupt configuration failed: -134
<err> gpio_keys: Pin 0 interrupt configuration failed: -134
```

`-134`는 Zephyr의 `-ENOTSUP`입니다. 원인은 이 보드의 하드웨어 배선에 있습니다.

- `user_button`은 `gpio_exp0`(PCAL6416A, I2C로 붙는 GPIO expander) 뒤에 물려 있습니다.
- `gpio_exp0`을 인터럽트 방식으로 쓰려면 expander 칩의 INT 핀이 MCU의 GPIO에 연결되어 있어야 하고, devicetree에는 이를 `int-gpios` 프로퍼티로 표현합니다.
- 그런데 **M4 쪽 base devicetree의 `gpio_exp0` 노드에는 `int-gpios`가 정의되어 있지 않습니다.** (M55 쪽 정의에는 `int-gpios = <&gpioa 3 GPIO_ACTIVE_LOW>;`가 있지만, M55는 이 랩을 포함한 모든 랩에서 I2C1을 disable하므로 애초에 의미가 없습니다.)
- `gpio_pcal64xxa.c` 드라이버는 `int-gpios`가 없는 인스턴스에 대해 `pin_interrupt_configure()`가 항상 `-ENOTSUP`을 반환하도록 구현되어 있습니다.

즉, **이 보드의 M4 쪽에서는 `gpio_exp0` 뒤에 달린 어떤 핀도 인터럽트 기반으로 이벤트를 받을 수 없다는 하드웨어적 제약**이 있습니다. 기본 설정의 `gpio-keys` 드라이버는 인터럽트 기반으로 동작하므로 이 제약에 그대로 부딪힙니다.

이를 위해 Zephyr의 `zephyr,gpio-keys` 바인딩(`zephyr/dts/bindings/input/gpio-keys.yaml`)에는 정확히 이런 상황을 위한 `polling-mode` 불리언 프로퍼티가 마련되어 있습니다. 이 옵션을 켜면 드라이버가 인터럽트를 걸지 않고, 대신 타이머로 주기적으로(기본 `debounce-interval-ms`인 30ms 간격) 핀 상태를 읽어 눌림/뗌을 판정합니다. M4 오버레이에 다음과 같이 추가되어 있습니다.

```dts
&buttons {
	polling-mode;
};
```

**이 랩에서 얻어갈 교훈**: devicetree 프로퍼티 하나를 잘못 빼먹으면 기능 자체가 아니라 "그 기능을 인터럽트로 쓸 수 있는지"가 하드웨어 배선에 의해 결정된다는 점, 그리고 Zephyr 드라이버들은 대체로 이런 제약을 우회할 수 있는 옵션(여기서는 `polling-mode`)을 미리 마련해두는 경우가 많다는 점입니다. 에러 코드(`-ENOTSUP`)와 드라이버 소스를 함께 읽으면 "왜 안 되는지"와 "어떻게 우회하는지"를 모두 찾아낼 수 있습니다.

## 아키텍처 및 코드 설명

### 전체 흐름

```
사용자가 SW8 버튼을 누름
        │
        ▼ (gpio_exp0, I2C, 폴링 30ms 주기)
Zephyr gpio-keys 드라이버 → Input 서브시스템 이벤트 발생
        │
        ▼
button_input_cb()  ── INPUT_CALLBACK_DEFINE()로 등록된 콜백 (근접 ISR 컨텍스트)
        │  press(value==1)만 걸러서 k_msgq_put(K_NO_WAIT)
        ▼
press_msgq (k_msgq, 4 엔트리)
        │
        ▼
Button_Task (워커 스레드, 우선순위 5)
        │  1) g_press_count 증가
        │  2) gpio_pin_toggle_dt(&led1)  — I2C, 스레드 컨텍스트라 안전
        │  3) mbox_send_dt(&tx_channel, ...)  — struct ipc_button_evt { seq } 전송
        ▼
                    (mbox, IPC)
        ▼
mbox_rx_callback()  ── M55 쪽, ISR 컨텍스트
        │  memcpy로 이벤트 복사 후 바로 LOG_INF (블로킹 작업 없으므로 워커 스레드 불필요)
        ▼
콘솔에 "Button event received, seq=N" 출력
```

### 공유 헤더 — `lab/include/ipc_common.h`

```c
struct ipc_button_evt {
	uint32_t seq;
};
```

Lab 01의 ping/pong 페이로드와 마찬가지로, 이 랩의 페이로드도 최소한으로 설계되어 있습니다. 버튼이 몇 번째로 눌렸는지를 나타내는 `seq` 하나만 담습니다. "버튼이 눌렸다"는 사실 자체는 mbox 메시지가 도착했다는 것 자체로 전달되므로, 페이로드에는 그 사실을 뒷받침하는 부가 정보(순번)만 있으면 충분합니다.

### M4(CLIENT) — `lab/remote/src/main.c`

**입력 콜백 등록과 필터링**

```c
static void button_input_cb(struct input_event *evt, void *user_data)
{
	uint8_t dummy = 1;

	ARG_UNUSED(user_data);

	/* Only handle press (1), ignore release (0) */
	if (evt->code != INPUT_KEY_0 || evt->value != 1) {
		return;
	}

	k_msgq_put(&press_msgq, &dummy, K_NO_WAIT);
}
INPUT_CALLBACK_DEFINE(NULL, button_input_cb, NULL);
```

`INPUT_CALLBACK_DEFINE(NULL, ...)`의 첫 번째 인자가 `NULL`이면 시스템에 있는 모든 입력 디바이스의 이벤트를 이 콜백이 받습니다(이 랩에서는 `buttons` 노드 하나뿐이므로 문제 없음). `evt->code`는 devicetree에서 `zephyr,code = <INPUT_KEY_0>;`로 지정한 값이고, `evt->value`는 1이면 눌림, 0이면 뗌입니다. 이 랩은 뗌 이벤트에는 관심이 없으므로 눌림만 걸러서 큐에 넣습니다. 큐에 넣는 값 자체는 의미가 없는 더미(`dummy = 1`)이며, "메시지가 큐에 존재한다"는 사실 자체가 신호입니다.

**워커 스레드에서의 실제 처리**

```c
while (1) {
	if (k_msgq_get(&press_msgq, &dummy, K_FOREVER) != 0) {
		continue;
	}

	g_press_count++;

	/* Visual feedback: toggle local LED1 (I2C, safe here since we're in thread context) */
	gpio_pin_toggle_dt(&led1);

	/* Send the event to M55 */
	out_evt.seq = g_press_count;
	mbox_msg.data = &out_evt;
	mbox_msg.size = sizeof(out_evt);

	ret = mbox_send_dt(&tx_channel, &mbox_msg);
	...
}
```

`Button_Task`는 큐가 빌 때 `K_FOREVER`로 블로킹 대기하다가, 이벤트가 들어오면 순서대로 (1) 순번 증가, (2) LED1 토글(I2C, 스레드 컨텍스트이므로 블로킹되어도 안전), (3) mbox 전송을 수행합니다. Lab 01에서 이미 익힌 `mbox_dt_spec` / `mbox_send_dt()` API가 그대로 재사용됩니다.

### M55(HOST) — `lab/src/main.c`

```c
static void mbox_rx_callback(const struct device *dev, mbox_channel_id_t channel_id,
			      void *user_data, struct mbox_msg *data)
{
	struct ipc_button_evt evt;

	...
	if (data->size < sizeof(evt)) {
		return;
	}
	memcpy(&evt, data->data, sizeof(evt));

	LOG_INF("Button event received, seq=%u", evt.seq);
}
```

M55는 LED1을 직접 켤 수 없으므로(하드웨어 소유권이 M4에 있음) 할 수 있는 일이 로그 출력뿐입니다. 코드 주석에도 있듯, 이 정도로 가벼운 작업은 콜백(ISR) 컨텍스트에서 바로 처리해도 무방하며, 별도로 워커 스레드나 메시지 큐를 둘 필요가 없습니다. 이는 앞서 "핵심 IPC 개념" 절에서 설명한 판단 기준 — 콜백 안의 작업이 블로킹 가능성이 있는지 — 을 그대로 보여주는 예시입니다.

## Devicetree 설정 설명

### M4 오버레이 — `lab/remote/boards/sr100_rdk_sr100_m4.overlay`

```dts
/* ipc0 shared-memory-size must match the M55 overlay in boards/ exactly. */
&ipc0 {
	shared-memory-size = <0x400>;
};

&buttons {
	polling-mode;
};
```

- `ipc0`의 `shared-memory-size`는 mbox가 사용하는 공유 메모리 영역 크기이며, M4/M55 양쪽 오버레이에서 반드시 동일한 값이어야 합니다(Lab 01에서 다룬 내용과 동일).
- `&buttons { polling-mode; };`는 앞서 설명한 대로, `gpio_exp0`에 `int-gpios`가 없어 인터럽트를 걸 수 없는 하드웨어 제약을 우회하기 위한 설정입니다.

### M55 오버레이 — `lab/boards/sr100_rdk_sr100_m55.overlay`

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

M4와 M55는 물리적으로 같은 I2C1 버스(SCL/SDA 핀)를 공유합니다. 두 코어가 동시에 같은 버스를 초기화하려 들면 부팅 시 충돌이 발생할 수 있으므로, M55 쪽에서는 `i2c1`을 통째로 `disabled`로 만들어 M4만 이 버스를 사용하도록 합니다. 부모 버스(`i2c1`)를 disable하는 것만으로는 부족한데, 자식 노드(`gpio_exp0`, `ov02c10`)는 자신의 `status`를 따로 지정하지 않으면 기본값인 `"okay"`를 유지하기 때문입니다. 그러면 빌드 시스템은 여전히 이 노드들을 "활성 인스턴스"로 간주해 드라이버를 컴파일하려 들고, `DT_BUS()`가 (disabled 상태인) `i2c1` 디바이스 핸들을 찾지 못해 빌드 에러가 발생합니다. 그래서 자식 노드들도 명시적으로 `disabled` 처리되어 있습니다. (이 내용은 Lab 01 하드웨어 테스트 중 처음 발견되어 이후 모든 랩에 공통 적용되었습니다.)

이 오버레이 덕분에 M55 쪽 코드에는 `gpio_exp0`, LED, 버튼에 접근하는 어떤 코드도 존재하지 않으며, 오직 mbox 수신과 로그 출력만 담당합니다.

## 빌드 방법

이 랩은 M4(remote, CLIENT)와 M55(host, HOST) 두 개의 독립 이미지로 구성됩니다. M55 이미지가 M4 바이너리를 포함하는 구조이므로, **M4를 먼저 빌드**해야 합니다.

```bash
# 1) M4(remote)
west build -p always -b sr100_rdk/sr100/m4 ./zephyr_ipc_sr110/02_button_pong/lab/remote -d m4

# 2) M55(host, M4 바이너리를 M4_BUILD로 포함)
west build -p always -b sr100_rdk/sr100/m55 ./zephyr_ipc_sr110/02_button_pong/lab -d m55 \
    -DCONFIG_SR100_RELEASE_M4_RESET=y -DM4_BUILD="../m4"
```

> **주의**: `M4_BUILD`는 M55 빌드 디렉토리(`m55/`) 기준 상대경로입니다. `m4/`와 `m55/`가 워크스페이스 루트의 형제 디렉토리라면 `../m4`가 맞습니다.

## 실행 및 결과 확인

두 이미지를 플래시하고 부팅하면 다음과 같은 동작을 확인할 수 있습니다.

1. SW8 버튼을 누를 때마다 M4 콘솔에 다음과 같은 로그가 출력되고, 동시에 LED1이 토글됩니다.
   ```
   [Button_Task] button pressed, seq=1 -> sent to M55
   ```
2. 곧이어 M55 콘솔에 다음 로그가 출력됩니다.
   ```
   Button event received, seq=1
   ```
3. 버튼을 계속 누르면 `seq` 값이 1씩 증가하며, M4와 M55 양쪽 로그의 `seq` 값이 항상 일치해야 합니다.

부팅 직후에는 양쪽 모두 "ready" 성격의 로그(`M4 ready, waiting for button presses.` / `Waiting for button events from M4.`)가 출력되며, 이후 버튼 입력이 있을 때까지 두 코어 모두 유휴 상태로 대기합니다.

## 정리

- Lab 02는 **소프트웨어가 발생시키는 이벤트(Lab 01)에서 실제 하드웨어 이벤트(버튼)로** 트리거를 바꾸어, mbox IPC가 진짜 디바이스 이벤트 통지에 쓰이는 모습을 보여주었습니다.
- 하드웨어 소유권(이 보드에서는 `gpio_exp0` = M4)이 IPC 메시지의 방향(M4 → M55)과 역할 분담(M4는 액추에이터+센서, M55는 관찰자)을 결정합니다.
- 인터럽트/콜백 컨텍스트(Input 콜백이든 mbox 콜백이든)에서는 **블로킹 가능한 작업 여부**를 기준으로, 즉시 처리할지(M55의 로그 출력) 워커 스레드로 넘길지(M4의 I2C LED 토글 + mbox 전송)를 판단해야 합니다.
- devicetree의 `int-gpios` 유무 같은 하드웨어 배선 정보가 드라이버 동작 모드(인터럽트 vs. `polling-mode`)를 결정하며, 에러 코드(`-ENOTSUP`)와 바인딩 문서를 함께 읽으면 우회 방법을 찾을 수 있습니다.

## 다음 랩 예고

Lab 03에서는 이번 랩에서 다룬 "M4 → M55 단방향 이벤트 통지"를 확장하여, 더 복잡한 페이로드나 양방향 상호작용을 다룰 예정입니다. Lab 02에서 익힌 "ISR/콜백 → 메시지 큐 → 워커 스레드" 패턴과 devicetree 오버레이 작성법은 앞으로의 모든 랩에서 계속 재사용되니, 이해가 되지 않는 부분이 있다면 다음으로 넘어가기 전에 이 문서를 다시 한 번 읽어보길 권장합니다.

---

문제가 발생했다면 → [`02_button_pong_troubleshooting_kr.md`](./02_button_pong_troubleshooting_kr.md) 참고

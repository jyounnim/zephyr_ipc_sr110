# Lab 03: Full-Duplex Ping-Pong

M55와 M4가 같은 mbox 채널 쌍의 tx/rx를 동시에 사용해, 서로 쉬지 않고 핑퐁을 주고받는 양방향(full-duplex) IPC 랩입니다.

## 학습 목표

- mbox의 tx 채널과 rx 채널을 **같은 스레드/코어에서 동시에** 사용하는 full-duplex 통신 구조를 이해한다.
- mbox 수신 콜백이 **ISR(인터럽트) 컨텍스트**에서 실행된다는 사실이 코드 설계에 어떤 제약을 주는지 이해한다.
- "ISR 콜백 → `k_msgq` → 워커 스레드"로 이어지는 3단 패턴을 실제 코드로 구현하고, 왜 이 패턴이 필요한지 설명할 수 있다.

## 이전 랩과의 연결

- **Lab 01**: M55 → M4 한 방향으로만 신호를 보내는 가장 단순한 mbox 통신이었습니다.
- **Lab 02**: M4의 버튼 입력을 M55로 알리는, 역시 한 방향(M4 → M55) 통신이었습니다.
- **Lab 03**: 이번에는 두 방향을 **동시에** 씁니다. M55가 ping을 보내면 M4가 받아서 LED0를 토글하고 pong으로 응답하며, M55는 pong을 받자마자 다음 ping을 보냅니다. 이 왕복이 끊임없이 반복되는 것이 이 랩의 핵심입니다. 지금까지는 "한쪽은 보내기만, 한쪽은 받기만" 했다면, 이제부터는 같은 `mbox_consumer` 노드의 `tx`/`rx`를 **양쪽 코어 모두** 동시에 사용합니다.

## 핵심 IPC 개념

### 1. Full-duplex IPC란?

Full-duplex(전이중)란 통신의 양쪽 끝이 **서로 독립적으로, 동시에** 송신과 수신을 할 수 있는 구조를 말합니다. 전화 통화처럼 양쪽이 동시에 말하고 들을 수 있는 것과 대비해서, 무전기처럼 한 번에 한쪽만 말할 수 있는 것을 half-duplex라고 부릅니다.

이 랩에서 M55와 M4는 각각:
- 자신의 **tx 채널**로 상대에게 메시지를 보내고,
- 동시에 자신의 **rx 채널**에 등록된 콜백으로 상대의 메시지를 받습니다.

두 채널은 서로 다른 mbox 채널(`ipc0`의 채널 0/1)이므로 물리적으로 독립적입니다. 그래서 "M55가 ping을 보내는 동작"과 "M4가 이전 pong에 대한 후속 처리를 하는 동작"이 시간적으로 겹쳐도 아무 문제가 없습니다 — 이것이 바로 full-duplex의 본질입니다. 이 랩의 핑퐁 자체는 순서대로 진행되지만(ping을 보내야 pong이 오고, pong이 와야 다음 ping을 보냄), 채널 구조 자체는 언제든 양방향 동시 전송을 지원합니다.

### 2. mbox 콜백은 절대 블로킹되면 안 된다

이 랩에서 가장 중요하게 다뤄야 할 개념입니다. 결론부터 말하면:

> **mbox 수신 콜백(`mbox_rx_callback`)은 ISR(인터럽트 서비스 루틴) 컨텍스트에서 실행되며, 이 안에서는 스레드를 재우거나 스케줄러 개입이 필요한 어떤 블로킹 호출도 해서는 안 됩니다.**

**왜 ISR에서 블로킹하면 안 되는가 (RTOS 일반론)**

RTOS의 인터럽트 컨텍스트는 "현재 실행 중이던 스레드를 잠깐 멈추고 끼어든" 상태입니다. 이 컨텍스트에는 애초에 "나 자신을 스케줄러 큐에 되돌려놓고 다른 스레드에게 CPU를 양보한다"는 개념이 성립하지 않습니다 — ISR은 스레드가 아니라 스레드 실행을 가로챈 예외 처리 루틴이기 때문입니다. 그런데 `k_msleep()`, `k_sem_take(K_FOREVER)`, `k_mutex_lock()` 같은 블로킹 API는 내부적으로 "현재 실행 흐름을 재우고 스케줄러에게 다음 실행 대상을 고르게 한다"는 동작을 전제로 합니다. ISR 안에서 이런 호출을 하면 커널은 "돌아갈 스레드 컨텍스트가 없는" 상태에 빠지고, 대부분의 RTOS(Zephyr 포함)는 이를 심각한 오류로 간주해 그 자리에서 시스템이 멈추거나(hang) 어서션 실패로 죽습니다.

**이 프로젝트에서 특히 치명적인 이유**

Zephyr의 기본 로깅 모드는 지연 처리(deferred) 방식입니다. `LOG_INF()` 등을 호출하면 실제 출력은 별도의 로깅 스레드가 버퍼에서 꺼내 나중에 UART로 내보냅니다. 그런데 ISR 안에서 커널이 멈춰버리면, 이미 버퍼에 쌓여 있던 로그(심지어 부팅 배너까지)를 내보낼 그 로깅 스레드조차 스케줄될 기회를 영영 잃습니다. 결과적으로 콘솔에는 **아무것도 출력되지 않고**, 겉보기엔 "보드가 아예 아무것도 안 한 것"처럼 보입니다. 원인 파악이 훨씬 어려워지는 이유가 바로 이것입니다 — 에러 로그조차 볼 수 없는 상태로 멈추기 때문입니다.

**이 랩에서 이 원칙이 왜 처음 중요해지는가**

Lab 01/02는 한 방향 통신이라 수신 측이 받은 신호를 단순히 로그로 남기거나 GPIO 하나를 토글하는 정도로 끝났습니다. 하지만 이 랩부터는 수신 콜백 뒤에 "일정 시간 대기 후 다음 메시지 전송"처럼 **의도적으로 지연이 필요한 로직**이 등장합니다(핑퐁 리듬을 눈으로 볼 수 있도록 0.5초 간격을 둠). 만약 이 지연(`k_msleep()`)을 콜백 안에서 직접 호출한다면, 정확히 위에서 설명한 이유로 시스템이 멈춥니다.

### 3. 표준 3단 패턴: ISR → k_msgq → 워커 스레드

그래서 이 랩(과 이후 모든 랩)은 다음 구조를 예외 없이 따릅니다.

1. **mbox 수신 콜백 (ISR 컨텍스트)**: 받은 메시지를 복사해 메시지 큐에 넣기만 하고 즉시 반환합니다. `k_msgq_put(..., K_NO_WAIT)`처럼 블로킹하지 않는 큐 삽입만 사용합니다.
2. **`k_msgq`**: ISR과 워커 스레드 사이의 안전한 통로 역할을 합니다. ISR 쪽은 절대 대기하지 않고(`K_NO_WAIT`), 워커 스레드 쪽은 메시지가 올 때까지 얼마든지 기다려도 됩니다(`K_FOREVER`) — 스레드 컨텍스트이므로 블로킹이 전혀 문제되지 않습니다.
3. **워커 스레드 (스레드 컨텍스트)**: 큐에서 메시지를 꺼내 실제 처리를 담당합니다. 여기서는 `k_msleep()`, GPIO 제어, 다음 메시지 전송 등 무엇을 해도 안전합니다.

이 랩의 M4(client) 코드를 보면 이 패턴이 그대로 구현돼 있습니다.

```c
static void mbox_rx_callback(const struct device *dev, mbox_channel_id_t channel_id,
			      void *user_data, struct mbox_msg *data)
{
	struct ipc_pingpong_msg msg;
	...
	memcpy(&msg, data->data, sizeof(msg));
	k_msgq_put(&rx_msgq, &msg, K_NO_WAIT);   /* ISR-safe: 큐에 넣고 즉시 반환 */
}

static void pong_task_entry(void *p1, void *p2, void *p3)
{
	struct ipc_pingpong_msg msg;
	...
	while (1) {
		if (k_msgq_get(&rx_msgq, &msg, K_FOREVER) != 0) {
			continue;
		}

		gpio_pin_toggle_dt(&led0);   /* I2C 접근 포함 — 스레드 컨텍스트에서만 안전 */
		LOG_INF("PING received, seq=%u -> LED0 toggled, replying PONG", msg.seq);

		mbox_msg.data = &msg;
		mbox_msg.size = sizeof(msg);
		mbox_send_dt(&tx_channel, &mbox_msg);
	}
}
```

M55(host) 쪽도 동일한 구조입니다. 다만 워커 스레드 안에서 `k_msleep(PINGPONG_DELAY_MS)`로 일부러 지연을 준 뒤 다음 ping을 보내는 부분이 이 랩에서 새로 추가된 로직입니다.

```c
static void mbox_rx_callback(const struct device *dev, mbox_channel_id_t channel_id,
			      void *user_data, struct mbox_msg *data)
{
	struct ipc_pingpong_msg msg;
	...
	memcpy(&msg, data->data, sizeof(msg));
	/* ISR-safe: no blocking calls here, just enqueue for the worker thread */
	k_msgq_put(&rx_msgq, &msg, K_NO_WAIT);
}

static void pong_task_entry(void *p1, void *p2, void *p3)
{
	struct ipc_pingpong_msg msg;
	...
	while (1) {
		if (k_msgq_get(&rx_msgq, &msg, K_FOREVER) != 0) {
			continue;
		}

		LOG_INF("PONG received, seq=%u", msg.seq);

		/* Pause here (thread context — perfectly fine to block) so the
		 * ping-pong rhythm is easy to watch on the console/LED. */
		k_msleep(PINGPONG_DELAY_MS);
		send_ping(msg.seq + 1);
	}
}
```

두 코드 모두 `mbox_rx_callback()`은 로그 한 줄과 `k_msgq_put()` 외에는 아무것도 하지 않고, 실제 "무엇을 할지"는 전부 `pong_task_entry()`라는 별도의 워커 스레드(`Pong_Task`)에서 처리합니다. 이 3단 구조는 이후 모든 랩에서 예외 없이 반복되는 이 커리큘럼의 표준 골격이니, 지금 확실히 익혀두는 것이 좋습니다.

> **참고**: mbox 송신 함수(`mbox_send_dt()`) 자체를 콜백 안에서 직접 호출해도 되는지에 대해서는 이 랩에서는 다루지 않습니다 — 이 보드의 mbox 백엔드에서 `mbox_send_dt()`도 특정 조건에서 블로킹될 수 있다는 사실이 이후 Lab 07에서 별도로 발견됩니다. 이 랩의 M4 코드는 어차피 pong 전송도 워커 스레드에서 하므로 해당 이슈와 무관하게 안전합니다.

## 아키텍처

```
M55 (host)                              M4 (client)
----------                              -----------
main()
  send_ping(1)  ----ping(seq)---------->  mbox_rx_callback()
                                             -> k_msgq_put()
                                                    |
                                                    v
                                           Pong_Task
                                             -> LED0 toggle
                                             -> mbox_send_dt(pong)
  mbox_rx_callback()  <----pong(seq)-----------------+
    -> k_msgq_put()
         |
         v
  Pong_Task
    -> k_msleep(500ms)
    -> send_ping(seq+1)  ----ping(seq+1)--------> (반복)
```

메시지 구조체(`lab/include/ipc_common.h`)는 양방향(ping/pong)에 동일하게 재사용됩니다.

```c
/* Shared by both directions: ping (M55->M4) and pong (M4->M55) both use this one struct. */
struct ipc_pingpong_msg {
	uint32_t seq;
};
```

seq 번호 하나만 실어 보내며, M4는 받은 seq를 그대로 pong에 담아 돌려주고, M55는 pong의 seq에 1을 더해 다음 ping을 보냅니다. 이 덕분에 콘솔 로그만으로도 왕복이 순서대로 진행되고 있는지 바로 확인할 수 있습니다.

## devicetree 설정

`ipc0`의 `shared-memory-size`는 M55/M4 오버레이 양쪽에서 반드시 같은 값이어야 합니다. 이 랩에서는 두 오버레이 모두 `0x400`으로 맞춰져 있습니다.

```dts
/* lab/boards/sr100_rdk_sr100_m55.overlay */
&ipc0 {
	shared-memory-size = <0x400>;
};
```

```dts
/* lab/remote/boards/sr100_rdk_sr100_m4.overlay */
&ipc0 {
	shared-memory-size = <0x400>;
};
```

또한 M55와 M4는 물리적으로 같은 I2C1 버스(SCL/SDA)를 공유하는 보드 배선을 가지고 있습니다. LED0/LED1이 매달린 `gpio_exp0`(PCA6416A)를 M4만 구동하도록, M55 오버레이에서 `i2c1`과 그 자식 노드들을 명시적으로 비활성화합니다.

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

부모 노드(`i2c1`)만 비활성화하는 것으로는 부족합니다 — 자식 노드는 자신의 `status`가 별도로 지정되지 않으면 기본값 `"okay"`를 유지하므로, 드라이버가 여전히 그 노드를 "활성 인스턴스"로 보고 컴파일에 포함시키려 합니다. 이 경우 `DT_BUS()`가 비활성화된 `i2c1` 핸들을 찾지 못해 빌드 에러가 나므로, 자식 노드도 함께 명시적으로 꺼줘야 합니다.

## 빌드 방법

```bash
# 1) M4(remote)
west build -p always -b sr100_rdk/sr100/m4 ./zephyr_ipc_sr110/03_full_duplex_ping_pong/lab/remote -d m4

# 2) M55(host, M4 바이너리를 M4_BUILD로 포함)
west build -p always -b sr100_rdk/sr100/m55 ./zephyr_ipc_sr110/03_full_duplex_ping_pong/lab -d m55 \
    -DCONFIG_SR100_RELEASE_M4_RESET=y -DM4_BUILD="../m4"
```

> **주의**: `M4_BUILD`는 M55 빌드 디렉토리(`m55/`) 기준 상대경로입니다. `m4/`와 `m55/`가 워크스페이스 루트의 형제 디렉토리라면 `../m4`가 맞습니다.

## 실행 및 결과 확인

보드에 플래시 후 두 코어의 콘솔을 함께 관찰하면:

- **M4**: LED0가 약 1초(왕복 0.5초 × 2) 주기로 계속 토글됩니다.
- **M55/M4 양쪽 콘솔**: PING/PONG의 seq 번호가 계속 증가하며 끊임없이 출력됩니다.

```
[M55] PING sent, seq=1
[M4 ] PING received, seq=1 -> LED0 toggled, replying PONG
[M55] PONG received, seq=1
[M55] PING sent, seq=2
[M4 ] PING received, seq=2 -> LED0 toggled, replying PONG
[M55] PONG received, seq=2
...
```

두 로그가 중단 없이 계속 이어지고 LED0가 일정한 주기로 토글되면 정상 동작입니다.

## 핵심 요약

- Full-duplex IPC는 두 채널(tx/rx)을 양쪽 코어가 동시에 독립적으로 쓸 수 있는 구조입니다.
- **mbox 수신 콜백은 ISR 컨텍스트에서 실행되므로 어떤 블로킹 호출도 해서는 안 됩니다.** ISR 안에서 블로킹하면 스케줄러가 개입할 대상 자체가 없어 커널이 멈추고, Zephyr의 지연 로깅 방식 때문에 이미 쌓인 로그조차 화면에 못 뜨는 채로 멈춥니다.
- 이를 피하기 위한 표준 패턴은 **ISR 콜백 → `k_msgq_put(K_NO_WAIT)` → 워커 스레드에서 `k_msgq_get(K_FOREVER)`로 실제 처리**입니다. 이 3단 구조는 이후 모든 랩에서 반복되는 이 커리큘럼의 기본 골격입니다.

## 다음 랩

Lab 04에서는 M55가 카운터 값을 주기적으로 보내고 M4가 이를 받아 LED를 여러 번 점멸시키는, 구조체 payload를 활용한 데이터 전달 랩으로 이어집니다.

---

문제가 발생했다면 → [`03_full_duplex_ping_pong_troubleshooting_kr.md`](./03_full_duplex_ping_pong_troubleshooting_kr.md) 참고

# Lab 08: Multi-Type Message Queue — 여러 종류 메시지 큐잉

M55가 6초마다 서로 다른 4개의 LED 패턴 커맨드를 대기 없이 연달아 보내고, M4는 `k_msgq`(깊이 8)에 도착 순서대로 쌓인 메시지를 하나씩 완주하며 처리하는, **연속 메시지 스트림 큐잉**을 다루는 예제입니다. 지금까지의 랩이 "신호 하나 → 반응 하나"의 단발성 패턴이었다면, 이 랩은 처음으로 "생산자가 소비자보다 훨씬 빠르게 여러 메시지를 쏟아내는" 상황을 의도적으로 만들어, `k_msgq`가 그 속도 차이를 큐로 흡수하는 과정을 눈으로 확인합니다.

## 학습 목표

이 랩을 마치면 다음을 할 수 있게 됩니다.

- 단발성 요청/응답이 아니라 **여러 종류의 메시지가 연속으로 도착하는 스트림**을 처리해야 하는 상황에서 `k_msgq` 기반 큐잉 패턴이 왜 필요한지 설명할 수 있다.
- `K_MSGQ_DEFINE`의 큐 depth(용량) 파라미터가 무엇을 의미하며, 생산자(M55)와 소비자(M4)의 처리 속도 차이를 고려해 이 값을 어떻게 설계해야 하는지 이해할 수 있다.
- 큐가 가득 찼을 때 `k_msgq_put(..., K_NO_WAIT)`가 어떤 결과를 반환하는지, 그리고 이를 ISR 콜백 안에서 절대 블로킹되지 않도록 처리하는 방법을 안다.
- 서로 다른 종류(태그가 붙은 메시지)를 하나의 큐와 하나의 워커 스레드에서 순서대로 처리하는 구조를 스스로 구현할 수 있다.
- `k_msgq_num_used_get()`으로 런타임에 큐의 현재 적체량을 관측하고, 이를 로그로 남겨 큐잉이 실제로 일어나고 있는지 검증할 수 있다.

## 이전 랩과의 연결

Lab 01~07까지는 대체로 "M55가 신호를 하나 보내면 M4가 그에 대응하는 동작 하나를 즉시 수행"하는 흐름이었고, 다음 신호는 이전 처리가 끝난 뒤에야 도착하도록 타이밍이 여유 있게 설계되어 있었습니다. 특히 Lab 07(Echo Service)에서는 `mbox` 콜백 안에서 어떤 블로킹 호출도(심지어 `mbox_send_dt()` 자체도) 절대 해서는 안 된다는 원칙이 실기 검증을 통해 확립되었습니다.

Lab 08은 이 원칙을 그대로 이어받으면서, 처음으로 **"소비자가 아직 이전 메시지를 처리 중인데 생산자가 다음 메시지를 벌써 보내는" 상황**을 의도적으로 만듭니다. M55는 4개의 패턴 커맨드를 사이에 아무 지연 없이 연속으로 보내는데, M4는 각 패턴을 완주하는 데 `200ms × 2 × repeat`(최소 1.2초, 최대 1.8초)가 걸립니다. 즉 M55의 전송 속도가 M4의 처리 속도를 훨씬 앞지르는 구조이며, 이 차이를 흡수하는 것이 바로 이 랩의 핵심 주제인 메시지 큐입니다.

## 핵심 IPC 개념 설명

### 왜 "단발성 응답"이 아니라 "큐잉"이 필요한가

지금까지의 랩에서는 메시지가 도착하면 그 자리에서(또는 워커 스레드로 넘겨) 바로 처리하고, 다음 메시지가 올 때쯤이면 이전 처리는 이미 끝나 있었습니다. 이런 구조에서는 사실 큐의 depth가 1이어도 별문제가 없습니다 — 메시지가 쌓일 일이 없기 때문입니다.

하지만 실제 시스템에서는 생산자와 소비자의 처리 속도가 항상 맞아떨어지지 않습니다. 이 랩처럼 한쪽이 "터뜨리듯" 여러 이벤트를 짧은 시간에 연달아 보내거나, 소비자 쪽 작업(여기서는 LED 패턴 실행)이 본질적으로 시간이 걸리는 경우, 미처리 메시지가 쌓이는 구간이 반드시 생깁니다. 이때 큐가 없다면 두 가지 나쁜 선택지만 남습니다.

1. 콜백(ISR)이 이전 메시지 처리가 끝날 때까지 **블로킹**한다 → ISR에서 블로킹은 금지된 동작이므로 이는 애초에 선택지가 아닙니다.
2. 처리하지 못한 메시지를 그냥 **버린다** → 이 랩처럼 4개의 서로 다른 패턴 커맨드가 각각 의미를 갖는 경우, 유실은 곧 기능 오류입니다.

`k_msgq`는 이 문제에 대한 세 번째 선택지, 즉 **"지금 당장 처리하지 못하는 메시지를 정해진 용량만큼 잠시 보관해 두었다가, 소비자가 준비되는 대로 순서대로 꺼내 쓴다"**는 완충(buffering) 메커니즘을 제공합니다. ISR(mbox 콜백)은 큐에 넣는 논블로킹 연산만 수행하면 되므로 여전히 안전하고, 소비자(워커 스레드)는 자기 속도대로 하나씩 처리하면 됩니다. 생산자와 소비자의 속도가 서로 달라도 되는, **비동기 파이프라인**이 만들어지는 것입니다.

### 큐 depth(용량) 설계가 왜 중요한가

`K_MSGQ_DEFINE(pattern_msgq, sizeof(struct ipc_pattern_msg), 8, 4)`에서 세 번째 인자 `8`이 바로 큐의 depth, 즉 동시에 보관할 수 있는 최대 메시지 개수입니다(네 번째 인자 `4`는 각 메시지의 정렬 바이트 수입니다). 이 값은 아무렇게나 정하면 안 되고, **생산자가 한 번에 몰아서 보낼 수 있는 최대 메시지 개수**와 **소비자가 그것을 다 처리하기까지 걸리는 시간 동안 추가로 도착할 수 있는 메시지 개수**를 함께 고려해서 정해야 합니다.

이 랩에서 M55는 한 주기(6초)에 4개의 패턴을 연달아 보냅니다. 큐 depth를 8로 넉넉하게 잡아 두었기 때문에, M4가 이전 주기의 메시지를 아직 다 처리하지 못한 상태에서 다음 주기의 4개가 또 들어와도(최악의 경우 최대 8개가 동시에 대기) 유실 없이 받아낼 수 있습니다. depth가 만약 4보다 작게(예: 3) 설정되어 있었다면, 한 주기 안에서조차 4번째 패턴이 큐에 들어가지 못하고 버려지는 상황이 발생했을 것입니다.

큐가 가득 찬 상태에서 `k_msgq_put(&pattern_msgq, &msg, K_NO_WAIT)`을 호출하면 블로킹하지 않고 즉시 `-ENOMSG`를 반환합니다. 이 랩의 콜백은 이 반환값을 별도로 검사하지 않지만(단순화를 위해), 실제 제품 코드라면 이 실패를 카운트하고 로그로 남겨 "큐가 실제로 넘쳤는지"를 운영 중에 관측할 수 있게 하는 것이 중요합니다 — depth를 얼마나 넉넉히 잡아야 하는지는 결국 이런 관측 데이터로 검증해야 하는 값이기 때문입니다.

### 이 랩이 이미 Lab 07의 교훈을 반영한 "모범 사례" 설계인 이유

Lab 07에서는 애초에 "가벼운 API라면 mbox 콜백(ISR) 안에서 직접 호출해도 된다"고 여겨졌던 예외가, 실기 검증 과정에서 `mbox_send_dt()` 자체가 이 보드에서 블로킹될 수 있다는 사실이 확인되며 전면 폐기되었습니다. 즉 "ISR 안에서는 큐에 넣는 것 외에 그 어떤 IPC 호출도 하지 말라"는 것이 이후 모든 랩에 적용되는 원칙이 되었습니다.

Lab 08은 이 원칙을 설계 단계부터 그대로 반영한 구조입니다. M4의 `mbox_rx_callback()`을 보면 하는 일이 다음 세 줄뿐입니다.

```c
memcpy(&msg, data->data, sizeof(msg));
k_msgq_put(&pattern_msgq, &msg, K_NO_WAIT);
```

이 콜백은 M55에게 **회신을 보내지 않습니다.** 이 랩은 애초에 M4→M55 응답 자체가 없는 **M55→M4 단방향 구조**로 설계되어 있어서, "콜백 안에서 `mbox_send_dt()`를 호출해야 하는가?"라는 질문 자체가 성립하지 않습니다. 회신할 데이터가 없으니 회신을 시도할 코드도 없고, 따라서 Lab 07에서 발견된 "`mbox_send_dt()`도 블로킹될 수 있다"는 위험에 애초에 노출되지 않는 것입니다. 콜백은 오직 큐에 넣기만 하고, 실제로 시간이 걸리는 작업(LED 패턴 실행, 다수의 블로킹 `k_msleep` 포함)은 전부 별도의 `Pattern_Task` 워커 스레드에서만 수행됩니다. 이런 의미에서 Lab 08은 "일부러 안전한 패턴만 골라 쓴" 랩이 아니라, **큐잉이라는 주제 자체가 이 원칙과 자연스럽게 맞아떨어지는 설계**라고 볼 수 있습니다.

## 아키텍처 / 코드 설명

### 메시지 구조체 (`lab/include/ipc_common.h`)

```c
enum ipc_pattern_id {
    PATTERN_BLINK_LED0  = 1,
    PATTERN_BLINK_LED1  = 2,
    PATTERN_ALTERNATE   = 3,
    PATTERN_BOTH_FLASH  = 4,
};

struct ipc_pattern_msg {
    uint32_t pattern_id; /* enum ipc_pattern_id */
    uint32_t repeat;     /* number of times to repeat the pattern */
};
```

`pattern_id`가 이 랩에서 "여러 종류의 메시지"를 구분하는 태그 역할을 합니다. `repeat`는 해당 패턴을 몇 번 반복할지를 담아, 같은 종류의 메시지라도 매번 다른 길이의 작업이 되도록 만듭니다(M4가 처리하는 데 걸리는 시간이 매번 달라져 큐 적체 양상을 더 다양하게 관찰할 수 있습니다).

### M55 (HOST) 쪽 — `lab/src/main.c`

M55는 이 랩에서 **메시지를 몰아서 보내는 생산자** 역할입니다.

```c
static void send_pattern(uint32_t pattern_id, uint32_t repeat)
{
    struct ipc_pattern_msg msg = {.pattern_id = pattern_id, .repeat = repeat};
    struct mbox_msg mbox_msg = {.data = &msg, .size = sizeof(msg)};

    mbox_send_dt(&tx_channel, &mbox_msg);
    LOG_INF("queued pattern_id=%u repeat=%u", pattern_id, repeat);
}
```

`main()`의 루프는 이 함수를 4번 연달아 호출한 뒤에야 6초를 잡니다.

```c
while (1) {
    send_pattern(PATTERN_BLINK_LED0, 3);
    send_pattern(PATTERN_BLINK_LED1, 3);
    send_pattern(PATTERN_ALTERNATE, 3);
    send_pattern(PATTERN_BOTH_FLASH, 2);

    k_msleep(6000);
}
```

4번의 `send_pattern()` 호출 사이에 의도적으로 아무 지연도 없습니다. `mbox_send_dt()`는 M4의 인터럽트를 발생시키고 곧바로 반환하는 호출이므로, 이 루프는 사실상 순식간에 4개의 메시지를 M4 쪽으로 밀어 넣습니다. M4가 이 4개를 실제로 다 처리하는 데는 (3+3+3+2)×2×200ms = 4.4초가 걸리므로, M4 입장에서는 "한꺼번에 몰려온 뒤 한참을 소화해야 하는" 전형적인 큐잉 상황이 매 주기 반복됩니다.

### M4 (CLIENT) 쪽 — `lab/remote/src/main.c`

M4는 **큐에서 순서대로 꺼내 소비하는** 역할이며, 앞서 설명한 ISR → `k_msgq` → 워커 스레드 패턴이 그대로 쓰입니다.

```c
K_MSGQ_DEFINE(pattern_msgq, sizeof(struct ipc_pattern_msg), 8, 4);

static void mbox_rx_callback(const struct device *dev, mbox_channel_id_t channel_id,
                              void *user_data, struct mbox_msg *data)
{
    struct ipc_pattern_msg msg;

    if (data->size < sizeof(msg)) {
        return;
    }
    memcpy(&msg, data->data, sizeof(msg));
    k_msgq_put(&pattern_msgq, &msg, K_NO_WAIT);
}
```

콜백은 크기 검증과 `memcpy`, `k_msgq_put` 세 가지 일만 하고 즉시 반환합니다. 앞서 설명했듯 이 콜백에는 M55로의 회신이 없으므로, ISR 안에서 실행되는 코드는 전부 논블로킹 연산뿐입니다.

실제 소비는 `Pattern_Task`라는 별도 워커 스레드에서 이루어집니다.

```c
static void pattern_task_entry(void *p1, void *p2, void *p3)
{
    struct ipc_pattern_msg msg;

    while (1) {
        if (k_msgq_get(&pattern_msgq, &msg, K_FOREVER) != 0) {
            continue;
        }
        LOG_INF("running pattern_id=%u repeat=%u (queue depth now=%u)",
                msg.pattern_id, msg.repeat, k_msgq_num_used_get(&pattern_msgq));
        run_pattern(&msg);
    }
}
```

이 스레드는 `K_FOREVER`로 큐에 새 메시지가 들어오길 기다리다가, 메시지를 하나 꺼내면 `k_msgq_num_used_get()`으로 **지금 이 순간 큐에 몇 개가 더 대기 중인지**를 함께 로그로 남긴 뒤, `run_pattern()`으로 해당 패턴을 완주할 때까지(여러 번의 `k_msleep(200)` 포함) 실행합니다. 이 함수는 스레드 컨텍스트에서 실행되므로 블로킹 호출을 자유롭게 써도 안전합니다. `run_pattern()`은 `pattern_id`에 따라 LED0만 깜빡이거나, LED1만 깜빡이거나, 두 LED를 번갈아 켜거나, 두 LED를 동시에 켜는 네 가지 동작을 각각 `repeat`번 반복합니다.

`main()`에서는 mbox 콜백을 등록·활성화한 뒤 `Pattern_Task` 스레드를 생성하고, 자신은 `k_sleep(K_FOREVER)`로 잠들어 실질적인 일은 전부 워커 스레드와 ISR에 맡기는 구조입니다.

## devicetree 설정 설명

이 랩의 오버레이 파일들은 IPC 자체와 관련해서는 기존 랩들과 동일한 최소한의 설정만 가지고 있습니다.

```dts
/* lab/boards/sr100_rdk_sr100_m55.overlay, lab/remote/boards/sr100_rdk_sr100_m4.overlay 공통 */
&ipc0 {
    shared-memory-size = <0x400>;
};
```

`shared-memory-size`는 M55와 M4 두 오버레이에서 반드시 동일한 값이어야 하는, IPC 공유 메모리 영역의 크기입니다. 이 랩에서 오가는 메시지(`struct ipc_pattern_msg`, 8바이트)는 여전히 작지만, 다른 랩들과 통일된 1KB 값을 그대로 유지하고 있습니다.

M55 쪽 오버레이에는 이 외에 `&i2c1`, `&gpio_exp0`, `&ov02c10`을 비활성화하는 설정이 추가로 들어 있습니다.

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

M4와 M55는 물리적으로 같은 I2C1 버스(LED0/LED1이 매달린 GPIO expander가 이 버스에 있음)를 공유합니다. 이 랩에서는 M4만 LED를 실제로 구동하므로, M55 쪽에서는 이 버스를 아예 갖지 않도록 명시적으로 비활성화해 부팅 시 버스 소유권 충돌을 막습니다. 이 이슈의 발견 경위는 [Lab 01 트러블슈팅 문서](../../01_hello_ipc/doc/01_hello_ipc_troubleshooting_kr.md)에 정리되어 있습니다.

## 빌드 방법

M4(remote) 이미지를 먼저 빌드하고, 그 결과물을 M55(host) 이미지가 참조하도록 빌드합니다. west 워크스페이스 루트에서 다음 순서로 진행합니다.

```bash
# 1) M4 (remote) 이미지 빌드
west build -p always -b sr100_rdk/sr100/m4 ./zephyr_ipc_sr110/08_message_queue/lab/remote -d m4

# 2) M55 (host) 이미지 빌드 — M4 바이너리를 포함
west build -p always -b sr100_rdk/sr100/m55 ./zephyr_ipc_sr110/08_message_queue/lab -d m55 \
    -DCONFIG_SR100_RELEASE_M4_RESET=y -DM4_BUILD="../m4"
```

`M4_BUILD`는 M55 빌드 디렉토리(`m55/`) 기준 **상대경로**로 해석됩니다. `m4/`와 `m55/`가 워크스페이스 루트의 형제 디렉토리로 만들어지는 이 구조에서는 `../m4`가 맞는 값입니다(자세한 배경은 [Lab 01 트러블슈팅 문서](../../01_hello_ipc/doc/01_hello_ipc_troubleshooting_kr.md) 참고).

## 실행 및 결과 확인

두 코어의 시리얼 콘솔을 각각 230400bps, 8N1로 엽니다. 정상 동작 시 다음과 같은 패턴이 반복됩니다.

- **M55 콘솔**: 6초마다 `queued pattern_id=N repeat=N` 형태의 로그가 4줄 연달아(거의 동시에) 출력됩니다 — 4개의 메시지를 지연 없이 밀어 넣고 있다는 뜻입니다.
- **M4 콘솔**: `running pattern_id=N repeat=N (queue depth now=N)`이 하나씩 순서대로(도착 순서 그대로) 출력되며, 각 로그가 찍힐 때마다 그 패턴이 완주(약 200ms × 2 × repeat)될 때까지 실제 LED0/LED1이 해당 패턴대로 동작합니다. 처음 로그의 `queue depth now`는 3(방금 꺼낸 1개를 뺀 나머지 3개) 근처로 찍혔다가, M4가 하나씩 처리해 나가면서 점점 줄어드는 것을 볼 수 있습니다 — 이 숫자의 변화가 곧 큐잉이 실제로 일어나고 있다는 직접적인 증거입니다.

## 정리 / 핵심 요약

이 랩에서는 M55가 4개의 서로 다른 LED 패턴 메시지를 지연 없이 연달아 보내고, M4가 `k_msgq`(depth 8)로 이를 받아 순서대로 하나씩 완주하는 구조를 구현했습니다. 핵심은 다음 세 가지입니다.

- 생산자와 소비자의 처리 속도가 다를 수밖에 없는 상황에서, `k_msgq`가 그 차이를 흡수하는 완충 역할을 한다.
- 큐 depth는 "생산자가 한 번에 몰아 보낼 수 있는 최대 개수"를 기준으로 설계해야 하며, 부족하면 `k_msgq_put(..., K_NO_WAIT)`이 실패(메시지 유실)로 이어진다.
- 이 랩의 mbox 콜백은 M55로의 회신이 없는 단방향 구조이기 때문에, Lab 07에서 확립된 "콜백 안에서는 큐에 넣는 것 외에 아무 IPC 호출도 하지 말라"는 원칙을 별도의 예외 처리 없이 자연스럽게 지킬 수 있었다.

## 다음 랩 예고

다음 랩(Lab 09)에서는 통신의 주체와 성격이 바뀝니다 — 이 랩까지는 M55가 M4에게 커맨드를 내리는 방향이었다면, Lab 09는 M4가 온보드 가속도계(MC3419)를 500ms 주기로 직접 샘플링해 그 값(x, y, z, seq)을 M55로 전송하는 **M4 → M55 텔레메트리(telemetry)** 구조를 다룹니다. 또한 M55 쪽은 수신한 값을 로그로 남기는 것 외에 별다른 블로킹 작업이 없어 워커 스레드 없이 ISR 콜백에서 곧바로 처리하는데, 이는 이 랩에서 큐가 반드시 필요했던 이유(느린 소비자)와 대비해 "큐가 필요 없는 경우"를 함께 이해하는 데 좋은 대조가 됩니다.

---

문제가 발생했다면 → `08_message_queue_troubleshooting_kr.md` 참고
</content>

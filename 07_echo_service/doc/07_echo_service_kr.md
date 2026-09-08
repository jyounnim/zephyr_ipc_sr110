# Lab 07: Echo Service — M55 ↔ M4 왕복 신뢰성 확인

M55가 2초마다 테스트 문자열을 M4에게 보내고, M4는 받은 내용을 그대로 되돌려줍니다. M55는 되돌아온 문자열을 원래 보낸 것과 비교해 `ECHO OK` 또는 `ECHO MISMATCH`를 로그로 출력합니다. LED나 버튼 같은 별도 하드웨어 없이, **오직 mbox IPC 왕복 자체의 신뢰성**만을 확인하는 랩입니다.

## 학습 목표

이 랩을 마치면 다음을 할 수 있게 됩니다.

- "받은 데이터를 그대로 돌려보낸다"는 echo 패턴이 IPC 경로의 무결성(전송 중 손상·유실 여부)을 검증하는 표준적인 방법인 이유를 설명할 수 있다.
- `mbox_send_dt()`처럼 겉보기에 가벼워 보이는 API도 콜백(ISR) 안에서 호출하면 안 되는 이유를 RTOS의 ISR 설계 원칙 관점에서 설명할 수 있다.
- mbox rx 콜백은 **예외 없이** "enqueue만 하고 즉시 반환"해야 하며, 실제 송신을 포함한 모든 처리는 워커 스레드로 미뤄야 한다는 이 커리큘럼의 최종 규칙을 스스로 코드에 적용할 수 있다.
- M4 쪽에서 `k_msgq`와 워커 스레드(`Echo_Task`)로 구성된 echo 서비스를 직접 읽고 그 구조를 재현할 수 있다.

## 이전 랩과의 연결

Lab 01에서 M55→M4 단방향 신호(ISR → `k_msgq` → 워커 스레드 패턴)를, 이후 랩들에서 다양한 방향과 페이로드의 IPC를 다뤄왔습니다. Lab 07은 그 흐름 위에서 "정확히 같은 내용이 왕복하는지"를 검증하는 가장 단순하지만 가장 근본적인 테스트로 돌아옵니다. 그리고 바로 이 랩에서, 지금까지 암묵적으로 지켜온 "ISR에서는 블로킹 금지"라는 원칙이 **`mbox_send_dt()` 자신에게도 예외 없이 적용된다**는 사실이 실기 테스트를 통해 명확히 확정되었습니다. 이 원칙은 이 랩 하나로 끝나지 않고, 이후 모든 랩에서 mbox 콜백을 작성할 때 지켜야 하는 커리큘럼 전체의 절대 규칙이 됩니다.

## 핵심 IPC 개념 설명

### echo 서비스 패턴이 IPC 신뢰성 테스트에 유용한 이유

Echo(반향) 서비스는 "받은 데이터를 가공 없이 그대로 되돌려준다"는 매우 단순한 동작을 합니다. 단순해 보이지만 네트워킹이나 IPC 스택을 검증할 때 오래전부터 쓰여온 표준적인 기법입니다. 이유는 명확합니다.

- 보내는 쪽이 원본을 그대로 갖고 있으므로, 돌아온 데이터와 **직접 바이트 단위로 비교**할 수 있습니다. 이는 다른 종류의 메시지(예: 이벤트 통지, 카운터 값)로는 얻기 어려운, 가장 강력한 형태의 왕복 검증입니다.
- 로직이 거의 없기 때문에(그대로 복사해서 돌려보내는 것뿐), 만약 값이 달라졌다면 그 원인은 애플리케이션 로직의 버그가 아니라 **IPC 경로 자체**(전송 크기 처리, 공유 메모리 오프셋, 타이밍, 큐잉 등)에 있다고 좁혀서 의심할 수 있습니다.
- 반복 전송(이 랩에서는 2초 주기)을 통해 "몇 번째 메시지부터 문제가 생기는가" 같은 패턴을 관찰할 수 있어, 일회성 테스트로는 드러나지 않는 상태 누적성 버그(큐 슬롯 소진, 리소스 누수 등)를 찾아내는 데 특히 유리합니다.

이 랩이 이 커리큘럼 안에서 정확히 그 역할을 했습니다 — 아래에서 설명할 "콜백에서 `mbox_send_dt()`를 직접 호출하면 안 된다"는 결정적인 설계 원칙이 바로 이 echo 왕복 테스트를 통해 드러났습니다.

### mbox_send_dt()도 콜백 안에서 호출하면 안 되는 이유

지금까지의 랩들에서 mbox rx 콜백은 항상 **ISR(인터럽트 서비스 루틴) 컨텍스트**에서 실행된다고 배웠고, 그래서 `k_msleep()`처럼 명백히 스레드를 재우는 함수는 절대 호출하면 안 된다는 원칙을 지켜왔습니다. 그런데 여기서 한 가지 질문이 남습니다 — "그럼 `mbox_send_dt()` 자체는 괜찮지 않을까? 결국 레지스터 몇 개를 쓰는 짧은 함수일 텐데."

이 질문에 대한 답이 바로 이 랩에서 실기로 확인되었고, 답은 **"아니오, 안전하지 않다"** 였습니다.

일반적인 RTOS 설계 원칙에서, ISR 안에서 호출해도 안전하다고 보장되는 함수는 명시적으로 "ISR-safe" 또는 "논블로킹"으로 문서화된 함수뿐입니다. 겉보기에 가벼워 보이는 함수라도, 그 내부 구현이 다음과 같은 이유로 실제로는 블로킹될 수 있습니다.

- 하드웨어 큐나 버퍼가 가득 찬 상태에서 호출되면, 드라이버가 내부적으로 "슬롯이 빌 때까지 대기"하는 로직을 가질 수 있습니다.
- 뮤텍스나 세마포어로 내부 상태를 보호하는 드라이버라면, 그 락을 다른 컨텍스트가 잡고 있는 동안 호출자가 대기하게 될 수 있습니다.
- 이전에 보낸 메시지가 아직 상대 코어에 의해 소비(drain)되지 않은 상태에서 새 전송을 시도하면, 그 소비가 끝날 때까지 기다리는 방식으로 구현되어 있을 수 있습니다.

이 보드의 mbox 백엔드가 정확히 이런 케이스였습니다 — API 문서만 보고 "가벼운 레지스터 쓰기이니 논블로킹일 것"이라 가정했지만, 실제로는 드라이버 구현에 따라 내부적으로 블로킹될 수 있는 함수였습니다. 그리고 ISR 컨텍스트에서 블로킹이 발생하면, 그 순간부터 해당 코어의 인터럽트 처리와 스케줄러 전체가 멈춰버립니다. Zephyr 커널은 ISR이 짧고 논블로킹이라는 전제 위에서 동작하기 때문에, 이 전제가 깨지면 복구할 방법이 없습니다.

**이로 인해 이 프로젝트의 최종 규칙이 다음과 같이 확정되었습니다.**

> mbox rx 콜백 안에서는 `k_msgq_put(..., K_NO_WAIT)`처럼 명시적으로 논블로킹이 보장된 호출만 사용하고, **그 외의 모든 처리 — `mbox_send_dt()` 호출을 포함해서 — 는 예외 없이 별도의 워커 스레드로 미룬다.**

이전까지는 "이 정도로 가벼운 호출은 콜백 안에서 예외적으로 허용해도 된다"는 여지가 문서에 남아 있었지만, 이 랩의 실기 검증 이후 그 예외는 완전히 폐기되었습니다. "이 API는 가벼워 보이니 괜찮을 것"이라는 추정에 기대지 않고, **콜백은 오직 enqueue만 한다**는 단일하고 예외 없는 규칙만 남긴 것입니다. 실기에서 실제로 시스템이 멈췄던 구체적인 과정과 진단 방법은 [트러블슈팅 문서](07_echo_service_troubleshooting_kr.md)에서 다룹니다.

## 아키텍처 / 코드 설명

### M55 (HOST) — `lab/src/main.c`

M55는 테스트 문자열 세 개(`test_strings[]`)를 순환하며 2초마다 하나씩 보내고, M4로부터의 응답을 콜백에서 즉시 비교합니다.

```c
static void mbox_rx_callback(const struct device *dev, mbox_channel_id_t channel_id,
                              void *user_data, struct mbox_msg *data)
{
    char buf[ECHO_MAX_LEN] = {0};
    size_t len = MIN(data->size, sizeof(buf) - 1);

    memcpy(buf, data->data, len);

    if (strcmp(buf, g_last_sent) == 0) {
        LOG_INF("ECHO OK: \"%s\"", buf);
    } else {
        LOG_WRN("ECHO MISMATCH: sent=\"%s\" got=\"%s\"", g_last_sent, buf);
    }
}
```

M55 쪽 콜백은 문자열 비교와 로그 출력만 하는, 시간이 매우 짧게 걸리는 작업이므로 ISR 안에서 직접 처리해도 안전합니다. 이는 앞서 설명한 "ISR에서는 블로킹 금지" 원칙에 위배되지 않는 예시입니다 — `strcmp`와 `LOG_INF`는 블로킹 호출이 아니기 때문입니다. (반면 M4 쪽은 `mbox_send_dt()`라는, 실제로 블로킹될 수 있는 호출을 콜백 안에서 하려 했기 때문에 문제가 된 것입니다.)

전송 루프는 단순합니다.

```c
while (1) {
    strncpy(g_last_sent, test_strings[idx % NUM_TEST_STRINGS], sizeof(g_last_sent) - 1);
    mbox_msg.data = g_last_sent;
    mbox_msg.size = strlen(g_last_sent) + 1; /* +1 to include the NUL terminator */

    LOG_INF("Sending: \"%s\"", g_last_sent);
    mbox_send_dt(&tx_channel, &mbox_msg);

    idx++;
    k_msleep(2000);
}
```

이 호출은 `main()`의 일반 스레드 컨텍스트에서 실행되므로, `mbox_send_dt()`가 내부적으로 블로킹되더라도 아무 문제가 없습니다. 블로킹이 위험한 이유는 어디까지나 **ISR 컨텍스트**에서 일어날 때뿐입니다.

### M4 (CLIENT) — `lab/remote/src/main.c`

M4 쪽이 이 랩의 핵심입니다. mbox 콜백은 수신한 바이트를 `struct echo_item`에 복사해 큐에 넣기만 합니다.

```c
K_MSGQ_DEFINE(echo_msgq, sizeof(struct echo_item), 4, 4);

static void mbox_rx_callback(const struct device *dev, mbox_channel_id_t channel_id,
                              void *user_data, struct mbox_msg *data)
{
    struct echo_item item;

    item.len = MIN(data->size, sizeof(item.data));
    memcpy(item.data, data->data, item.len);

    /* ISR-safe: no blocking calls here, just enqueue for the worker thread */
    k_msgq_put(&echo_msgq, &item, K_NO_WAIT);
}
```

`k_msgq_put`을 `K_NO_WAIT`로 호출한다는 점이 핵심입니다 — 큐가 가득 차 있어도 절대 대기하지 않고 즉시 실패를 반환하므로, 콜백은 어떤 상황에서도 블로킹되지 않습니다. 이 콜백 안에는 `mbox_send_dt()` 호출이 전혀 없습니다.

실제 회신은 별도로 만든 `Echo_Task` 워커 스레드에서만 이뤄집니다.

```c
static void echo_task_entry(void *p1, void *p2, void *p3)
{
    struct echo_item item;
    struct mbox_msg reply;

    LOG_INF("[Echo_Task] Started on M4");

    while (1) {
        if (k_msgq_get(&echo_msgq, &item, K_FOREVER) != 0) {
            continue;
        }

        LOG_INF("rx %u bytes, echoing back", (unsigned int)item.len);

        /* mbox_send_dt() may block on this board's mbox backend -- that's
         * fine here, we're in thread context, not an ISR. */
        reply.data = item.data;
        reply.size = item.len;
        mbox_send_dt(&tx_channel, &reply);
    }
}
```

`Echo_Task`는 큐에 새 항목이 들어올 때까지 `K_FOREVER`로 잠들어 있다가, 항목이 도착하면 깨어나 `mbox_send_dt()`를 호출합니다. 이 호출이 내부적으로 블로킹되더라도, `Echo_Task`는 일반 스레드이므로 Zephyr 스케줄러가 다른 스레드(로깅 스레드 등)를 계속 정상적으로 실행할 수 있습니다 — ISR과 스레드의 결정적인 차이입니다.

`main()`은 콜백 등록/활성화와 `Echo_Task` 생성을 마친 뒤 아무 일도 하지 않고 잠듭니다.

```c
k_thread_create(&echo_task_data, echo_task_stack,
                K_THREAD_STACK_SIZEOF(echo_task_stack),
                echo_task_entry, NULL, NULL, NULL,
                ECHO_TASK_PRIORITY, 0, K_NO_WAIT);
k_thread_name_set(&echo_task_data, "Echo_Task");
```

이렇게 **콜백(ISR) = enqueue 전용, 워커 스레드(`Echo_Task`) = 실제 처리 전용**으로 역할을 완전히 분리하는 것이, 이 랩에서 확정된 이 커리큘럼의 최종 규칙입니다. `mbox_send_dt()`처럼 이름만 보면 가벼워 보이는 함수라도 예외를 두지 않고 반드시 워커 스레드에서만 호출한다는 점을 다시 한번 기억하세요.

## devicetree 설정 설명

M55/M4 두 오버레이 모두 `ipc0`의 `shared-memory-size`를 동일하게 맞춥니다.

```dts
&ipc0 {
    shared-memory-size = <0x400>;
};
```

이 값은 두 코어가 공유하는 IPC 메모리 영역의 크기이며, 한쪽만 다르게 설정하면 메모리 레이아웃이 어긋나 통신이 깨집니다. 이 랩에서 오가는 문자열은 최대 `ECHO_MAX_LEN`(64바이트)이므로 1KB면 충분히 여유롭습니다.

M55 쪽 오버레이(`lab/boards/sr100_rdk_sr100_m55.overlay`)에는 이 외에도 `&i2c1`, `&gpio_exp0`, `&ov02c10`을 비활성화하는 설정이 들어 있습니다. 이 랩은 LED나 카메라를 전혀 사용하지 않지만, M4와 M55가 물리적으로 같은 I2C1 버스를 공유하는 이 보드의 하드웨어 구조 때문에 M55 쪽에서 버스 소유권 충돌이 나지 않도록 전 랩 공통으로 비활성화해 두었습니다 (자세한 배경은 [Lab 01 트러블슈팅 문서](../../01_hello_ipc/doc/01_hello_ipc_troubleshooting_kr.md) 참고).

## 빌드 방법

west 워크스페이스 루트에서 M4(remote)를 먼저 빌드한 뒤, 그 결과물을 포함해 M55(host)를 빌드합니다.

```bash
# 1) M4 (remote) 이미지 빌드
west build -p always -b sr100_rdk/sr100/m4 ./zephyr_ipc_sr110/07_echo_service/lab/remote -d m4

# 2) M55 (host) 이미지 빌드 — M4 바이너리를 M4_BUILD로 포함
west build -p always -b sr100_rdk/sr100/m55 ./zephyr_ipc_sr110/07_echo_service/lab -d m55 \
    -DCONFIG_SR100_RELEASE_M4_RESET=y -DM4_BUILD="../m4"
```

`M4_BUILD`는 M55 빌드 디렉토리(`m55/`) 기준 상대경로입니다. `m4/`와 `m55/`가 워크스페이스 루트의 형제 디렉토리 구조라면 `../m4`가 맞습니다 (자세한 원인은 [Lab 01 트러블슈팅 문서](../../01_hello_ipc/doc/01_hello_ipc_troubleshooting_kr.md) 참고).

## 실행 및 결과 확인

두 코어의 시리얼 콘솔을 각각 연결해 다음 로그를 확인합니다.

- **M55 콘솔**: 2초마다 `Sending: "..."`가 출력된 뒤, 정상 왕복 시 `ECHO OK: "..."`가 이어서 출력됩니다. 만약 내용이 어긋나면 `ECHO MISMATCH: sent="..." got="..."`가 출력됩니다.
- **M4 콘솔**: 부팅 시 `[Echo_Task] Started on M4`가 한 번 출력되고, 이후 메시지를 받을 때마다 `rx N bytes, echoing back`이 반복 출력됩니다.

정상 동작 시 M55와 M4 양쪽 로그가 2초 주기로 끊김 없이 계속 이어지는 것을 확인할 수 있습니다.

## 정리 / 핵심 요약

- Echo 패턴(받은 것을 그대로 되돌려주기)은 애플리케이션 로직을 배제하고 IPC 경로 자체의 신뢰성만을 순수하게 검증하는 유용한 테스트 기법입니다.
- **`mbox_send_dt()`도 예외 없이 콜백(ISR) 안에서 직접 호출하면 안 됩니다.** 이름이나 겉모습만으로 "가벼우니 논블로킹일 것"이라 추정해서는 안 되며, 드라이버 구현에 따라 내부적으로 블로킹될 수 있습니다.
- mbox rx 콜백의 유일한 책임은 `k_msgq_put(..., K_NO_WAIT)`으로 데이터를 큐에 넣고 즉시 반환하는 것이며, `mbox_send_dt()` 호출을 포함한 나머지 모든 처리는 반드시 별도의 워커 스레드(`Echo_Task`)에서 이뤄져야 합니다.
- 이 규칙은 이 랩 하나에 국한되지 않고, 이후 모든 랩에서 mbox 콜백을 작성할 때 지켜야 하는 이 커리큘럼의 최종 원칙입니다.

## 다음 랩 예고

다음 랩(Lab 08)에서는 단일 메시지 왕복을 넘어, 여러 메시지가 연속으로 쌓일 수 있는 상황에서의 `k_msgq` 기반 메시지 큐 처리를 더 본격적으로 다룹니다. 이 랩에서 확립한 "콜백은 enqueue만, 처리는 워커 스레드"라는 원칙 위에서, 큐가 가득 찼을 때의 동작이나 여러 메시지를 순서대로 소비하는 패턴 등을 확장해서 살펴보게 됩니다.

---

이 설계에 이르기까지의 실제 디버깅 과정이 궁금하다면 → `07_echo_service_troubleshooting_kr.md` 참고 (실기에서 실제로 시스템이 멈췄던 사례를 다룹니다)
</content>

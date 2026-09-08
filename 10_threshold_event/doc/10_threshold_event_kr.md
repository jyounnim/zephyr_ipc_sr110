# Lab 10: 임계값 이벤트 알림 (Threshold Event Notification)

매 샘플을 다 보내는 대신, 가속도계 값이 **임계값을 넘었을 때만** M4가 M55에게 이벤트를 보내는 조건부/이벤트 기반 IPC 패턴을 익힙니다.

## 학습 목표

- 폴링/주기 전송(polling)과 이벤트 기반(event-driven) 전송의 차이와 트레이드오프를 이해한다.
- 센서 원시값이 아니라 **baseline 대비 편차**로 판단해야 하는 이유를 이해하고, 부팅 시 캘리브레이션을 직접 설계해본다.
- 디바운스(debounce)로 짧은 시간 내 중복 이벤트를 억제하는 방법을 익힌다.
- 커맨드 수신과 주기 작업(폴링)을 **하나의 스레드**에서 `k_msgq` 타임아웃으로 함께 처리하는 패턴을 익힌다.

## 이전 랩과의 연결: "매 샘플 전송"에서 "조건부 전송"으로

Lab 09(`09_accel_telemetry`)에서는 M4가 가속도계를 일정 주기로 읽어 **매번** M55로 전송했습니다. M55는 그 값을 그대로 로그로 찍기만 했습니다. 이 방식은 구현이 단순하고 M55가 항상 최신 값을 갖고 있다는 장점이 있지만, 값이 거의 안 바뀌는 구간에서도 계속 mbox 트래픽과 로그가 발생한다는 단점이 있습니다.

Lab 10은 같은 센서(MC3419)를 쓰면서 M4 쪽 로직만 바꿉니다: 매 샘플을 여전히 100ms마다 폴링하지만, **그 값이 의미 있는 변화(임계값 초과)를 나타낼 때만** M55로 이벤트를 보냅니다. M55 쪽 코드는 오히려 더 단순해집니다 — 부팅 시 임계값을 1회 설정해두고, 그 뒤로는 이벤트가 도착할 때만 반응하면 됩니다.

## 핵심 IPC 개념

### 1. 이벤트 기반 vs 폴링/주기 전송

| | 폴링/주기 전송 (Lab 09) | 이벤트 기반 (Lab 10) |
|---|---|---|
| 전송 시점 | 정해진 주기마다 무조건 | 조건이 만족될 때만 |
| 대역폭 | 값의 변화와 무관하게 일정 | 평소엔 거의 0, 이벤트 시에만 소비 |
| 수신 측 부담 | 매번 파싱/처리해야 함(값이 그대로여도) | 의미 있는 상황에서만 깨어남 |
| 최신 상태 파악 | 항상 최신값을 갖고 있음 | 마지막 이벤트 이후 "조용함 = 임계값 이하"라고 가정해야 함 |
| 놓친 이벤트 | 다음 주기에 곧 복구됨 | mbox 전송 실패 등으로 이벤트 자체를 놓치면, 별도 재확인 없이는 수신 측이 알 방법이 없음 |

일반적으로 값이 자주/연속적으로 필요하거나(예: 실시간 제어 루프) 놓친 데이터가 치명적인 경우엔 폴링/주기 전송이 유리하고, 반대로 "평소엔 조용하고 특정 상황에서만 반응하면 되는" 경보/알림성 데이터(온도 이상, 충격 감지, 버튼 이벤트 등)는 이벤트 기반이 자원을 훨씬 아낍니다. 다만 이벤트 기반은 "마지막으로 조용했던 뒤로 시스템이 살아있는가"를 보장하지 않으므로, 실무에서는 종종 하트비트(heartbeat)를 별도로 함께 두어 "이벤트가 없다 = 정상"과 "이벤트가 없다 = 링크가 끊겼다"를 구분합니다 (이 커리큘럼 뒤쪽 랩에서 다룰 주제입니다).

### 2. baseline 캘리브레이션 후 편차 비교 — 왜 절대값이 아니라 상대값을 봐야 하는가

가속도계는 정지 상태에서도 중력가속도(~1g)를 항상 어느 한 축에 싣고 있습니다. 만약 "원시 magnitude가 임계값을 넘는가"로만 판단하면, 가만히 세워둔 보드조차 중력 성분만으로 이미 임계값 근처/이상의 값을 내보내게 되어 정지 상태에서도 계속 오탐(false positive) 이벤트가 발생합니다. 즉, 우리가 실제로 감지하고 싶은 것은 "센서가 지금 얼마나 큰 절대값을 가리키는가"가 아니라 "**평소(기준 상태)와 비교해 얼마나 달라졌는가**"입니다.

이 랩에서는 다음과 같은 설계 패턴을 씁니다:

1. 부팅 직후 약 1초간(`CALIB_SAMPLES`회) 센서를 반복 샘플링해 X/Y/Z 평균을 구하고, 이를 **baseline**으로 저장한다.
2. 이후의 모든 샘플은 원시값이 아니라 `현재값 - baseline`(편차, deviation)을 임계값과 비교한다.

이렇게 하면 중력 오프셋과 부팅 시점의 고정 기울기가 baseline에 흡수되어 상쇄되고, 남는 편차는 그 이후에 실제로 발생한 "움직임"만을 반영하게 됩니다. 이 baseline은 **부팅 시 한 번만 찍는 고정 스냅샷**이며, 계속 갱신되는 적응형(adaptive) 필터가 아닙니다 — 그래서 캘리브레이션 구간(부팅 후 약 1초) 동안 보드를 움직이면 그 움직임 자체가 baseline에 섞여 들어가 이후 판단이 왜곡됩니다.

이 "baseline 대비 편차 비교" 패턴은 가속도계에만 국한되지 않는 일반적인 설계 기법입니다. 절대 세기 자체보다 "정상 상태에서 얼마나 벗어났는가"가 중요한 모든 센서 응용(온도 드리프트, 압력, 조도, 자이로 바이어스 등)에 동일하게 적용할 수 있습니다.

### 3. 디바운스(debounce) — 왜 필요한가

임계값을 살짝 넘나드는 값이 짧은 시간 안에 여러 번 샘플링되면, 매 샘플마다 이벤트를 보내는 것은 낭비이고 수신 측 로그도 의미 없이 늘어납니다. 이 랩은 이벤트를 보낸 시각을 기록해두고, 마지막 이벤트로부터 `DEBOUNCE_MS`(500ms) 이내에는 아무리 임계값을 넘어도 다시 이벤트를 보내지 않습니다. 즉 "같은 움직임/충격"을 여러 개의 이벤트로 중복 보고하지 않고, 사람이 보기에 자연스러운 단위(하나의 사건 = 하나의 이벤트)로 묶어줍니다.

### 4. 커맨드 처리와 주기 샘플링을 한 스레드에서 `k_msgq` 타임아웃으로 함께 처리하기

이 랩의 M4 쪽 `Threshold_Task`는 별도의 스레드를 두 개 두지 않고, 하나의 워커 스레드가 두 가지 일을 동시에 처리합니다.

```c
while (1) {
    /* Process a command if one is waiting; otherwise time out after
     * POLL_PERIOD_MS and treat that as the sampling tick */
    if (k_msgq_get(&cmd_msgq, &cmd, K_MSEC(POLL_PERIOD_MS)) == 0) {
        if (cmd.cmd == IPC10_CMD_SET_THRESHOLD) {
            threshold_milli_g = cmd.threshold;
            LOG_INF("[Threshold_Task] threshold updated to %d", threshold_milli_g);
        }
        continue;
    }

    /* k_msgq_get() timed out -> no command arrived within POLL_PERIOD_MS,
     * so this iteration is a normal sampling tick */
    ...
}
```

`k_msgq_get()`에 `K_MSEC(POLL_PERIOD_MS)`(100ms) 타임아웃을 주면, 큐에 커맨드가 들어와 있으면 즉시 그것을 반환하고, 아무것도 없으면 100ms 뒤 타임아웃으로 반환합니다. 이 타임아웃 자체를 "100ms마다 한 번씩 센서를 폴링하라"는 신호로 재활용하는 것이 이 패턴의 핵심입니다. 그 결과:

- 별도의 타이머나 두 번째 스레드 없이, 커맨드가 도착하면 다음 폴링 tick까지 기다리지 않고 즉시(최대 지연 0에 가깝게) 처리됩니다.
- 커맨드가 없는 평소에는 정확히 폴링 주기로 센서를 읽습니다.
- 스레드가 하나뿐이므로 `threshold_milli_g` 같은 공유 상태에 별도의 락(lock)이 필요 없습니다 — 항상 같은 스레드 안에서만 읽고 쓰기 때문입니다.

## 아키텍처 요약

- **M55 (host, `lab/src/main.c`)**: 부팅 시 `IPC10_CMD_SET_THRESHOLD` 커맨드로 임계값(기본 2000 milli-g)을 M4에 1회 설정하고, 이후에는 `rx_cb()`에서 이벤트가 도착할 때만 로그를 남깁니다.
- **M4 (client, `lab/remote/src/main.c`)**: `Threshold_Task` 하나가 커맨드 처리와 100ms 폴링을 겸합니다. 부팅 직후 `calibrate_baseline()`으로 baseline을 찍고, 이후 매 샘플의 baseline 대비 편차(magnitude)가 임계값을 넘고 디바운스 구간을 지났을 때만 `mbox_send_dt()`로 이벤트를 보냅니다.

`calibrate_baseline()`:

```c
static void calibrate_baseline(int32_t *base_x, int32_t *base_y, int32_t *base_z)
{
    int64_t sum_x = 0, sum_y = 0, sum_z = 0;
    int good_samples = 0;
    int32_t x, y, z;

    for (int i = 0; i < CALIB_SAMPLES; i++) {
        if (read_accel_milli(&x, &y, &z) == 0) {
            sum_x += x;
            sum_y += y;
            sum_z += z;
            good_samples++;
        }
        k_msleep(POLL_PERIOD_MS);
    }
    ...
    *base_x = (int32_t)(sum_x / good_samples);
    *base_y = (int32_t)(sum_y / good_samples);
    *base_z = (int32_t)(sum_z / good_samples);
}
```

`CALIB_SAMPLES`(10) × `POLL_PERIOD_MS`(100ms) = 약 1초 동안 샘플을 모아 평균을 낸 뒤, 그 결과를 이후 모든 판단의 기준(baseline)으로 씁니다.

이벤트 판단 부분:

```c
int32_t dx = x - base_x;
int32_t dy = y - base_y;
int32_t dz = z - base_z;
int32_t mag = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy) + (dz < 0 ? -dz : dz);

int64_t now = k_uptime_get();
if (mag > threshold_milli_g && (now - state.last_event_ms) > DEBOUNCE_MS) {
    struct ipc10_event_msg evt = {.x = x, .y = y, .z = z, .seq = ++seq};
    struct mbox_msg mbox_msg = {.data = &evt, .size = sizeof(evt)};

    mbox_send_dt(&tx_channel, &mbox_msg);
    state.last_event_ms = now;
    ...
}
```

`mag`은 baseline과의 편차를 절대값 합(L1 norm 유사 형태)으로 근사한 값입니다. 이 값이 임계값을 넘고, 마지막 이벤트 이후 `DEBOUNCE_MS`가 지났을 때만 실제로 mbox 이벤트를 보냅니다.

### mbox 콜백은 절대 블로킹 금지

M55/M4 양쪽 `rx_cb()` 모두 즉시 반환합니다 — M4는 커맨드를 `k_msgq_put(..., K_NO_WAIT)`으로 큐에 넣기만 하고, M55는 이벤트 내용을 로그로 남기기만 합니다. 실제 처리(가속도계 폴링/캘리브레이션, 임계값 갱신, `mbox_send_dt()` 호출까지)는 전부 `Threshold_Task` 워커 스레드에서 이루어지므로, 이 랩은 Lab 03/Lab 07에서 확립된 "mbox 콜백은 절대 블로킹하지 않는다"는 원칙을 그대로 따릅니다.

## devicetree 설정

`ipc0` shared-memory-size는 M4/M55 양쪽 오버레이에서 반드시 동일해야 합니다(`0x400`).

M4 오버레이(`lab/remote/boards/sr100_rdk_sr100_m4.overlay`)에서는 가속도계 노드를 활성화합니다:

```
&mc3479 {
	status = "okay";
};
```

MC3419 가속도계의 devicetree 노드 레이블은 `accel0`이 아니라 **`mc3479`**이며, 기본 status가 `"disabled"`이므로 이렇게 명시적으로 켜줘야 합니다.

M55 오버레이(`lab/boards/sr100_rdk_sr100_m55.overlay`)에서는 M4와 M55가 물리적으로 공유하는 I2C1 버스 충돌을 피하기 위해 `&i2c1`(및 그 하위 노드)을 disable합니다 — I2C1은 M4만 사용합니다.

## 빌드 방법

```bash
# 1) M4(remote)
west build -p always -b sr100_rdk/sr100/m4 ./zephyr_ipc_sr110/10_threshold_event/lab/remote -d m4

# 2) M55(host, M4 바이너리를 M4_BUILD로 포함)
west build -p always -b sr100_rdk/sr100/m55 ./zephyr_ipc_sr110/10_threshold_event/lab -d m55 \
    -DCONFIG_SR100_RELEASE_M4_RESET=y -DM4_BUILD="../m4"
```

`M4_BUILD`는 M55 빌드 디렉터리(`m55/`) 기준 상대경로입니다. `m4/`와 `m55/`가 워크스페이스 루트의 형제 디렉터리라면 `../m4`가 맞습니다.

## 실행 및 결과 확인

두 이미지를 플래시하고 보드를 재부팅하면:

- **M4 콘솔**: 부팅 직후 `[Threshold_Task] calibrating baseline (~1s, keep the board still)...`가 출력되고, 약 1초 뒤 `[Threshold_Task] baseline x=.. y=.. z=.. (from 10 samples)`가 뒤따릅니다. M55로부터 커맨드를 받으면 `threshold updated to 2000`, 임계값을 넘는 움직임이 감지되면 `event sent seq=N mag=N (dev x=.. y=.. z=..)`가 출력됩니다.
- **M55 콘솔**: 부팅 시 `threshold set to 2000 milli-g`가 1회 출력됩니다. 이후 보드를 흔들어 임계값을 넘길 때만 `THRESHOLD EVENT seq=N x=.. y=.. z=..`가 찍히며, 가만히 두면 캘리브레이션 이후 아무 로그도 뜨지 않는 것이 정상입니다.

> **주의**: 캘리브레이션 중(부팅 후 약 1초간)은 보드를 움직이지 마세요. 이 구간에 움직이면 그 움직임 자체가 baseline에 섞여 들어가고, 이후 판단 기준이 왜곡됩니다.

## 정리

- Lab 09의 "매 샘플 전송"과 달리, Lab 10은 "조건을 만족할 때만" 이벤트를 보내는 방식으로 IPC 트래픽과 수신 측 처리 부담을 크게 줄입니다.
- 센서 값은 원시값이 아니라 부팅 시 캘리브레이션한 **baseline과의 편차**로 판단해야 정지 상태에서의 오탐을 막을 수 있습니다. 이 패턴은 가속도계 외의 다양한 센서 응용에도 일반화할 수 있습니다.
- 디바운스는 하나의 물리적 사건이 여러 개의 중복 이벤트로 보고되는 것을 막아줍니다.
- 커맨드 처리와 주기 폴링을 `k_msgq` 타임아웃 하나로 합쳐 단일 스레드에서 처리하면, 별도 타이머/두 번째 스레드 없이도 반응성과 락 없는 단순함을 동시에 얻을 수 있습니다.

## 다음 랩 예고

Lab 11은 디스플레이(OLED) 출력을 다루며, 지금까지 콘솔 로그로만 확인하던 이벤트/데이터를 화면에 직접 표시하는 방법을 익힙니다.

---

문제가 발생했다면 → [`10_threshold_event_troubleshooting_kr.md`](./10_threshold_event_troubleshooting_kr.md) 참고

# Lab 14: Heartbeat Watchdog

## 학습 목표

- 주기적 비콘(heartbeat) 메시지 하나만 한 방향으로 흐르는 가장 단순한 IPC 채널을 설계한다.
- M55가 `k_msgq_get()`을 **유한 타임아웃**과 함께 호출해 "메시지가 왔는지"와 "얼마나 기다렸는지"를 동시에 판단하는 패턴을 익힌다.
- 태그된 유니온이 항상 정답은 아니라는 것을 확인한다 — 메시지 종류가 하나뿐이면 굳이 `type` 태그를 붙이지 않는다.
- M4가 실제로 멎지 않고도(콘솔은 계속 살아 있는 채로) heartbeat 중단 상황을 재현하는 방법을 익힌다.
- Lab 13에서 확정된 "M4-로컬 vs M55-전달" 설계 원칙을 이번엔 **모든** 커맨드에 적용해 본다.

## 이전 랩과의 연결

Lab 14는 새 하드웨어를 전혀 추가하지 않습니다. 사용하는 것은 딱 두 가지뿐입니다.

1. M4 콘솔 UART(Lab 13에서 폴링 방식으로 검증 완료된 `uart_poll_in()` 커맨드 입력 경로) — 그대로 재사용.
2. mbox 채널(Lab 01부터 모든 랩이 사용해 온 `ipc0`) — 그대로 재사용.
3. M55의 ST7789V3 TFT(Lab 11/12/13과 동일한 raw-SPI 드라이버) — 그대로 재사용.

이 랩은 Lab 12/13의 AHT20/BMP280 센서를 전혀 건드리지 않습니다. 이번 랩의 주제는 센서 텔레메트리가 아니라 heartbeat/watchdog IPC 패턴 그 자체이기 때문입니다.

## 왜 "진짜 행(hang)"을 만들지 않았는가

Watchdog의 본래 목적은 "M4가 죽었는지"를 감지하는 것입니다. 그런데 교실 시연을 위해 M4를 실제로 크래시시키거나 무한루프에 빠뜨리는 것은 몇 가지 문제가 있습니다.

- 반복 가능한 데모가 되지 않습니다 (매번 다시 플래시해야 함).
- M4를 진짜로 멈추면 관찰하고 싶은 콘솔 로그도 같이 죽습니다.

그래서 이 랩은 **heartbeat를 보내는 스레드만** N초 동안 일시정지시키는 커맨드(`1 <1..60>`)를 제공합니다. M4의 나머지 부분(콘솔, 커맨드 처리)은 정상적으로 계속 동작합니다. 이것은 의도된 단순화입니다 — M55 입장에서는 "M4가 진짜로 멈췄다"와 "M4가 heartbeat 전송만 일시정지했다"를 구분할 방법이 전혀 없습니다. 채널에 침묵이 흐른다는 사실만 보이기 때문에, 이 단순화로도 M55의 타임아웃 감지 로직을 충실하게 시연할 수 있습니다.

## 핵심 개념

### 1. 메시지 종류가 하나뿐이면 태그된 유니온을 쓰지 않는다

Lab 06과 Lab 13은 한 mbox 채널에 여러 종류의 메시지(센서 데이터, 커맨드, 응답 등)를 흘려보내야 했기 때문에 `type` 필드로 태그된 유니온(`ipc13_msg`)을 사용했습니다. Lab 14는 M4 → M55 한 방향으로, 딱 한 종류의 메시지(heartbeat)만 흐릅니다. 이럴 땐 태그가 아무 역할도 하지 못하므로, 페이로드 구조체를 그대로 보냅니다.

```c
/* ipc_common.h -- 태그 없음, 딱 하나의 메시지 종류만 존재 */
struct ipc14_heartbeat_payload {
	uint32_t seq;
};
```

**교훈**: 태그된 유니온은 "여러 메시지 종류가 실제로 채널을 공유할 때"만 값어치를 합니다. Lab 13처럼 3종 메시지가 오갈 땐 태그가 필요했지만, 이 랩처럼 1종뿐이면 태그는 코드만 복잡하게 만들 뿐입니다.

### 2. `k_msgq_get()`을 유한 타임아웃으로 호출해 "감시"를 구현한다

M55의 `Watchdog_Task`는 다음과 같이 동작합니다.

```c
int ret = k_msgq_get(&hb_msgq, &hb, K_MSEC(WATCHDOG_CHECK_PERIOD_MS));
if (ret == 0) {
	/* heartbeat 도착 -- 정상 */
} else if (ever_seen && state == WD_OK &&
           (k_uptime_get() - last_seen_ms) >= WATCHDOG_TIMEOUT_MS) {
	/* WATCHDOG_TIMEOUT_MS 이상 heartbeat가 없었음 -- 타임아웃 */
}
```

`K_FOREVER`로 블로킹하면 "메시지가 안 왔다"는 사실 자체를 관찰할 방법이 없습니다. 반대로 `K_NO_WAIT`로 폴링하면 CPU를 계속 태우게 됩니다. `K_MSEC(N)` 같은 유한 타임아웃은 그 중간 지점 — "최대 N ms만 기다렸다가, 안 오면 깨어나서 경과 시간을 직접 계산한다" — 을 제공합니다. `WATCHDOG_CHECK_PERIOD_MS`(200ms)는 감시 주기이고, `WATCHDOG_TIMEOUT_MS`(1200ms, heartbeat 주기 300ms의 4배)는 실제로 타임아웃을 선언하는 문턱값입니다. 이 둘을 분리해 둔 이유는, 감시 주기를 짧게 잡아도 타임아웃 판정 자체는 "충분히 여러 번 heartbeat를 놓쳤을 때만" 내려지도록 하기 위해서입니다.

### 3. 상태 머신: WD_WAITING → WD_OK → WD_TIMEOUT

```c
enum wd_state { WD_WAITING, WD_OK, WD_TIMEOUT };
```

- `WD_WAITING`: 부팅 직후, 아직 heartbeat를 한 번도 못 받은 상태 (회색 "WAIT").
- `WD_OK`: heartbeat를 받고 있는 정상 상태 (녹색 "OK").
- `WD_TIMEOUT`: `WATCHDOG_TIMEOUT_MS` 이상 침묵이 이어진 상태 (빨간색 "TIMEOUT"). heartbeat가 다시 도착하면 즉시 `WD_OK`로 복귀합니다.

타임아웃이 발생할 때마다 `trips` 카운터를 증가시켜 TFT에 "지금까지 몇 번 타임아웃이 있었는지"도 함께 표시합니다.

### 4. 모든 커맨드가 M4-로컬 — Lab 13과의 대비

Lab 13에서는 커맨드 1/2/3이 M55로 전달(mbox 왕복)되고, 커맨드 4(로그 on/off)만 M4에서 로컬로 처리됐습니다. Lab 14는 그 반대입니다 — **모든** 커맨드가 M4 로컬입니다. heartbeat 전송을 일시정지/재개/조회하는 것은 순전히 M4 자신의 상태(`pause_until_ms`)를 다루는 일이라, mbox를 거칠 이유가 전혀 없기 때문입니다.

| 커맨드 | 의미 | M55로 전달? |
|---|---|---|
| `1 <1..60>` | heartbeat 전송을 N초간 일시정지 (행 시뮬레이션) | 아니오 (M4 로컬) |
| `2` | 일시정지 즉시 해제, heartbeat 재개 | 아니오 (M4 로컬) |
| `3` | 현재 상태(RUNNING / PAUSED, 남은 시간) 조회 | 아니오 (M4 로컬) |

### 5. 64비트 타임스탬프는 `atomic_t`가 아니라 `k_mutex`로 보호한다

Lab 13의 `display_mode` 플래그는 단순한 정수라서 `atomic_t`로 충분했습니다. 이 랩의 `pause_until_ms`는 `k_uptime_get()`이 반환하는 64비트(`int64_t`) 값입니다. 이 플랫폼에서 `atomic_t`는 64비트 값을 원자적으로 담지 못하므로, 대신 `k_mutex pause_lock`으로 `Uart_Cmd_Task`(쓰기)와 `Heartbeat_Task`(읽기) 간의 접근을 보호합니다.

### 6. 실제 M4 리셋 액션은 구현하지 않는다

Watchdog 패턴을 완성하려면 원래 "타임아웃 감지 → M4 리셋 트리거"까지 있어야 하지만, 이 커리큘럼을 설계하는 단계에서 M4를 런타임에 재시작시키는 API(`CONFIG_SR100_RELEASE_M4_RESET` 등)가 실기에서 확인되지 않았습니다. 검증되지 않은 API를 추측으로 넣는 대신, 이 랩은 **감지 + 표시**까지만 구현합니다 — TFT에 타임아웃 상태와 누적 횟수를 보여주는 것으로 watchdog의 핵심 관찰 포인트는 충분히 시연됩니다.

## 아키텍처 다이어그램

```
M4 (Cortex-M4)                              M55 (Cortex-M55)
───────────────                              ─────────────────
Heartbeat_Task                               Watchdog_Task
  |                                            |
  | pause_until_ms == 0?                       | k_msgq_get(K_MSEC(200))
  |   yes -> mbox_send_dt(seq++) ---- mbox ---> rx_cb() -> k_msgq_put()
  |   no  -> skip this cycle                   |
  | k_msleep(300ms)                            | 받으면: WD_OK, TFT SEQ 갱신
  |                                            | 못 받으면: 경과시간 >= 1200ms?
Uart_Cmd_Task                                  |            -> WD_TIMEOUT, trips++
  | uart_poll_in() 10ms 폴링                   | TFT에 상태(WAIT/OK/TIMEOUT) 표시
  | "1 N"/"2"/"3" -> handle_command()          |
  |   (전부 M4 로컬, mbox 사용 안 함)           |
```

## 핀 연결 (이 랩에서 실제로 사용하는 전체 배선 — Lab 11/12/13과 동일, 새 배선 없음)

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

이 연결은 Lab 13에서 검증한 것과 완전히 동일합니다. 이 랩에서는 센서(I2C0)를 전혀 쓰지 않으므로 M4 오버레이가 Lab 12/13보다 오히려 더 단순합니다.

## devicetree 설정

M4 오버레이(`remote/boards/sr100_rdk_sr100_m4.overlay`)는 `ipc0` shared-memory-size 설정만 남기고 I2C0 관련 설정을 모두 제거했습니다 — 이 랩은 센서를 쓰지 않기 때문입니다. M55 오버레이(`boards/sr100_rdk_sr100_m55.overlay`)는 Lab 13과 동일한 TFT/SPI0 설정을 그대로 유지합니다.

## prj.conf

- `lab/remote/prj.conf` (M4): `CONFIG_I2C=y`가 빠졌습니다(센서 미사용). UART 커맨드 입력은 Lab 13과 동일하게 별도 Kconfig 없이 기본 `CONFIG_SERIAL`만으로 동작합니다.
- `lab/prj.conf` (M55): Lab 13과 동일 — SPI/GPIO 활성화, M55 콘솔 UART 비활성화.

## 빌드 방법

```bash
# M55 (host)
west build -b sr100_rdk/sr100/m55 labs/14_heartbeat_watchdog/lab

# M4 (remote/client)
west build -b sr100_rdk/sr100/m4 labs/14_heartbeat_watchdog/lab/remote
```

## 실행 및 결과 확인

1. M4 콘솔(J24, 230400bps)과 M55 콘솔(J14 USB-C, 115200bps)을 모두 열어둡니다.
2. 두 코어 모두 플래시 후 리셋하면, M4는 300ms마다 heartbeat를 보내기 시작하고 M55의 TFT에는 처음엔 회색 "WAIT", 첫 heartbeat 수신 후 곧바로 초록색 "OK"가 표시됩니다.
3. M4 콘솔에 `3`을 입력해 상태를 확인합니다 — `[M4] status: RUNNING (heartbeat active)`가 출력되어야 합니다.
4. M4 콘솔에 `1 5`를 입력해 5초간 heartbeat를 일시정지합니다 — `[M4] heartbeat PAUSED for 5s (simulated hang) -- watch M55's TFT`가 출력됩니다. M55의 TFT를 지켜보면, 마지막 heartbeat로부터 약 1200ms(`WATCHDOG_TIMEOUT_MS`) 후 상태가 빨간색 "TIMEOUT"으로 바뀌고 TRIPS 카운터가 1 증가합니다.
5. 5초가 지나면 M4가 자동으로 재개하고, 다음 heartbeat가 M55에 도착하는 즉시 TFT는 다시 초록색 "OK"로 돌아옵니다.
6. `1 5`가 끝나기 전에 `2`를 입력하면 즉시 재개됩니다 — `[M4] heartbeat RESUMED`가 출력됩니다.
7. `1 70`처럼 범위를 벗어난 값을 입력하면 `[M4] pause seconds must be 1..60`이 출력되고 아무 동작도 하지 않습니다.

## 정리

Lab 14는 새 하드웨어 없이, IPC 채널을 감시하는 가장 기본적인 패턴 — 유한 타임아웃을 가진 `k_msgq_get()`으로 "침묵"을 능동적으로 감지하는 것 — 을 실기로 확인했습니다. 태그된 유니온이 항상 필요한 것은 아니라는 점, 그리고 모든 조작자 커맨드가 항상 상대 코어로 전달되어야 하는 것도 아니라는 점을 Lab 13과 대비해 확인했습니다. 실제 M4 리셋 트리거는 검증된 API가 없어 이번 랩의 범위 밖으로 남겨두었습니다.

**실기 검증 완료.** 커맨드 `1 <1..60>`(일시정지)/`2`(재개)/`3`(상태조회) 전부와, M55 TFT의 WAIT→OK→TIMEOUT 상태 전환 및 TRIPS 카운트 증가가 실제 하드웨어에서 정상 동작함을 확인했습니다.

## 참고

- Lab 13 (`13_uart_bridge`) — 이 랩이 재사용하는 `uart_poll_in()` 폴링 커맨드 입력 패턴의 원본, 그리고 인터럽트 기반 RX가 이 UART에서 왜 동작하지 않는지에 대한 트러블슈팅 문서.
- Lab 11 (`11_display_basic` 또는 해당 랩 이름) — 이 랩이 그대로 재사용하는 ST7789V3 raw-SPI 드라이버의 원본 및 레벨시프터/SPI0 핀 공유 관련 상세 설명.

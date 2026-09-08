# Lab 16: Capstone Gateway — 전체 패턴 통합

## 학습 목표

- 이 커리큘럼 전체에서 확립한 패턴들 — raw I2C/SPI 드라이버 직접 구현(Lab 11/12), 다중 센서 폴백과 이중 장애 지표(Lab 12), 능동 복구(Lab 12), 커맨드가 M4-로컬인지 M55로 릴레이되는지를 커맨드마다 따로 판단하는 원칙(Lab 13), 유한 타임아웃 기반 워치독(Lab 14), 상태 변화시에만 전송하는 Always-On/Wake 저전력 동기화(Lab 15) — 를 하나의 태그된 유니온(tagged union) IPC 프로토콜 위에서 동시에 굴려본다.
- 서로 다른 주기·서로 다른 성격을 가진 4개의 M4 스레드(환경 센서 감시, 모션 감지, 하트비트, 커맨드 입력)가 하나의 mbox tx 채널을 안전하게 공유하는 법(뮤텍스 직렬화)을 확인한다.
- M55 쪽에서는 여러 종류의 메시지를 **하나의 큐, 하나의 스레드**가 전부 처리하면서도, 그중 한 메시지 종류(하트비트)의 "부재"만 별도로 감시하는 설계를 구현한다 — Lab 14의 유한 타임아웃 워치독과 Lab 15의 상태-변화-시-전송 이벤트 처리가 한 스레드 안에서 공존할 때 어떤 트레이드오프가 생기는지 직접 확인한다.

## 이전 랩과의 연결

Lab 16은 새 하드웨어를 전혀 추가하지 않습니다. 이 랩에서 쓰는 모든 것은 이전 랩에서 이미 배선하고 검증한 것입니다.

1. AHT20+BMP280 콤보 모듈(M4, I2C0) — Lab 12/13/15와 동일한 배선, 동일한 raw I2C 드라이버.
2. 온보드 가속도계 MC3419(M4, I2C1) — Lab 09/10과 동일한 배선, 동일한 Zephyr 센서 서브시스템 사용법(ODR+FULL_SCALE 설정 포함).
3. M4 콘솔 UART의 폴링 커맨드 입력(M4, `uart_poll_in()`) — Lab 13/14/15에서 검증된 패턴 그대로.
4. ST7789V3 TFT(M55, SPI0) — Lab 11/12/13/14/15와 동일한 raw-SPI 드라이버.
5. mbox 채널(`ipc0`) — 모든 랩에서 재사용.

## 이 랩이 이전 랩들과 근본적으로 다른 점

지금까지의 랩은 대부분 "센서 하나(또는 하나의 축)"와 "이벤트 종류 하나"를 다뤘습니다. Lab 16은 그 반대입니다: 서로 독립적으로 동작하는 4개의 M4 스레드가 각자의 페이스로 mbox에 메시지를 보내고, M55는 그 메시지들을 **하나의 채널, 하나의 태그된 유니온**으로 받아 종류별로 다르게 반응해야 합니다. 이 랩의 핵심 질문은 "새 센서를 어떻게 읽는가"가 아니라 **"이미 검증된 여러 패턴을 하나의 시스템 안에서 충돌 없이 조합하려면 무엇을 조율해야 하는가"**입니다.

## 핵심 개념

### 1. 태그된 유니온으로 4가지 메시지 종류를 다중화

```c
enum ipc16_msg_type {
	IPC16_MSG_ENV = 0,
	IPC16_MSG_MOTION,
	IPC16_MSG_HEARTBEAT,
	IPC16_MSG_STATUS,
};

struct ipc16_msg {
	uint8_t type;
	union {
		struct ipc16_env_payload env;
		struct ipc16_motion_payload motion;
		struct ipc16_heartbeat_payload heartbeat;
		struct ipc16_status_payload status;
	} payload;
};
```

Lab 14/15는 메시지 종류가 정확히 하나뿐이라 태그가 필요 없었지만("태그는 여러 메시지 종류가 실제로 채널을 공유할 때만 값어치가 있다"), 이 랩은 Lab 06/13처럼 여러 종류가 실제로 한 채널을 공유하므로 태그가 다시 필요합니다.

### 2. 4개의 M4 스레드가 하나의 tx 채널을 공유 — `tx_lock`으로 직렬화

`Env_Task`(200ms 주기), `Motion_Task`(100ms 주기), `Heartbeat_Task`(1000ms 주기), `Uart_Cmd_Task`(커맨드 `4` 입력 시) 모두 `mbox_send_dt()`를 호출할 수 있습니다. 네 스레드 중 어느 둘이 동시에 전송을 시도해도 한쪽의 전송이 다른 쪽과 뒤섞이지 않도록, Lab 13에서 확립한 `k_mutex tx_lock`으로 모든 전송을 직렬화합니다.

```c
static void send_msg(struct ipc16_msg *msg)
{
	struct mbox_msg mbox_msg = {.data = msg, .size = sizeof(*msg)};

	k_mutex_lock(&tx_lock, K_FOREVER);
	mbox_send_dt(&tx_channel, &mbox_msg);
	k_mutex_unlock(&tx_lock);
}
```

### 3. ENV/MOTION은 "상태 변화시에만 전송" (Lab 15), HEARTBEAT는 "무조건 주기 전송" (Lab 14) — 한 시스템 안의 서로 다른 전송 철학

`Env_Task`는 AHT20+BMP280을 200ms마다 계속 읽지만, mbox 전송은 온도가 임계값을 넘거나(또는 센서 오류 상태가 바뀌거나) 하는 **실제 상태 변화가 있을 때만** 일어납니다(Lab 15 원칙). `Motion_Task`도 동일한 원칙을 가속도계에 적용합니다(Lab 10의 베이스라인 캘리브레이션 + Lab 15의 상태-변화-시-전송).

반면 `Heartbeat_Task`는 정반대입니다 — 아무 일이 없어도 1초마다 **무조건** 전송합니다(Lab 14 원칙). 이 차이는 우연이 아닙니다: ENV/MOTION은 "무슨 일이 일어났는지"를 알리는 것이 목적이라 상태 변화가 없으면 보낼 것이 없지만, HEARTBEAT는 "M4가 아직 살아있다"는 사실 자체를 증명하는 것이 목적이라, 정확히 규칙적으로 오지 않으면 그 부재를 감지할 방법이 없어집니다.

### 4. M55: 하나의 큐, 하나의 스레드가 4종 메시지 + 워치독 타이머를 함께 처리

```c
int ret = k_msgq_get(&msgq, &msg, K_MSEC(WATCHDOG_CHECK_PERIOD_MS));

if (ret == 0) {
	switch (msg.type) {
	case IPC16_MSG_ENV: /* TFT 갱신 */ break;
	case IPC16_MSG_MOTION: /* TFT 갱신 */ break;
	case IPC16_MSG_HEARTBEAT: /* 워치독 OK로 전이 */ break;
	case IPC16_MSG_STATUS: /* TFT 갱신 */ break;
	}
	continue;
}
/* 타임아웃 -- 하트비트가 WATCHDOG_TIMEOUT_MS 이상 안 왔는지 확인 */
```

Lab 15는 `K_FOREVER`로 무기한 블로킹했습니다("메시지 부재에 의미가 없으므로"). 이 랩은 그럴 수 없습니다 — HEARTBEAT의 부재 자체가 감시 대상이기 때문입니다. 그래서 `k_msgq_get()`은 Lab 14와 같은 **유한 타임아웃**(`WATCHDOG_CHECK_PERIOD_MS` = 500ms)을 씁니다. 메시지가 도착하면(`ret == 0`) 태그로 분기해 처리하고, 타임아웃이 나면(`ret != 0`) 마지막으로 하트비트를 본 시각과 지금 시각의 차이가 `WATCHDOG_TIMEOUT_MS`(4000ms, 하트비트 주기의 4배) 이상인지 검사합니다. ENV/MOTION/STATUS 메시지가 도착하는 것은 워치독 타이머를 리셋시키지 않습니다 — 오직 HEARTBEAT만 리셋시킵니다. 서로 다른 것을 재는 두 지표를 섞지 않기 위해서입니다.

### 5. 트레이드오프를 숨기지 않는다: Lab 15의 "완전한 저전력 대기" vs 이 랩의 "주기적 타이머 확인"

Lab 15의 M55는 아무 일도 없으면 `K_FOREVER`로 블로킹해 Zephyr의 idle 스레드가 CPU를 WFI 저전력 대기로 보내도록 두었습니다. 이 랩은 워치독을 위해 500ms마다 한 번씩 반드시 깨어나 시계를 확인해야 하므로, Lab 15만큼 깊은 저전력 대기에 머무를 수 없습니다. 이것은 버그가 아니라 **설계상의 트레이드오프**입니다 — 안전 감시(워치독)를 원한다면 어느 정도의 주기적 깨어남은 그 대가로 받아들여야 합니다. 이 랩은 그 트레이드오프를 감추지 않고 그대로 드러냅니다.

### 6. 커맨드 6개 중 5개는 M4-로컬, 1개만 M55로 릴레이 — Lab 13의 원칙을 다시 한번

| 커맨드 | 의미 | M55로 전달? |
|---|---|---|
| `1 <celsius>` | ENV(온도) 임계값 설정 | 아니오 (M4 로컬, Lab 15와 동일) |
| `2 <milli-g>` | MOTION(가속도) 임계값 설정 | 아니오 (M4 로컬, Lab 10과 동일) |
| `3` | M4가 알고 있는 모든 상태(ENV/MOTION/하트비트/업타임)를 M4 콘솔에 출력 | 아니오 (M4 로컬, Lab 14와 동일) |
| `4` | 지금 시점의 전체 스냅샷을 M55에 `IPC16_MSG_STATUS`로 즉시 push | **예 — 이 랩에서 유일하게 릴레이되는 커맨드** |
| `5 <seconds>` | `Heartbeat_Task`만 N초 동안 일시정지(하트비트 전송 중단) — Lab 14의 커맨드 `1`과 동일 | 아니오 (M4 로컬) |
| `6` | 일시정지된 하트비트를 즉시 재개 — Lab 14의 커맨드 `2`와 동일 | 아니오 (M4 로컬) |

임계값 설정과 로컬 조회(1/2/3)는 M4 자신의 상태만 다루므로 M55와 무관합니다. `4`만 예외인 이유는, "지금 M55의 화면에 최신 스냅샷을 강제로 반영하고 싶다"는 것 자체가 M55에게 전달해야만 의미가 있는 요청이기 때문입니다. 커맨드마다 "이게 정말 상대 코어까지 가야 하는가"를 따로 판단해야 한다는 Lab 13의 원칙이 이 랩에서 6개 커맨드 중 정확히 1개에만 적용되는 것으로 다시 확인됩니다.

`5`/`6`은 워치독을 시연하기 위한 커맨드입니다. **M4와 M55는 물리적으로 분리된 두 개의 보드가 아니라 하나의 SoC 안에 있는 두 개의 코어이므로, M4만 따로 전원을 끊거나 M4만 따로 리셋하는 것은 애초에 불가능합니다.** 그래서 Lab 14와 마찬가지로, `Heartbeat_Task`라는 스레드 하나만 N초 동안 멈추는 방식으로 "행(hang)"을 흉내 냅니다 — M4 콘솔(및 `Env_Task`/`Motion_Task`/`Uart_Cmd_Task`를 포함한 M4의 나머지 전부)은 계속 살아있는 채로, 오직 하트비트 전송만 멈춥니다. M55 입장에서는 mbox로 하트비트가 안 오는 것 외에는 아무 차이가 없으므로, 워치독 타임아웃 감지 로직을 시연하는 데는 이 방식으로 충분합니다.

## 아키텍처 다이어그램

```
M4 (Cortex-M4)                                        M55 (Cortex-M55)
────────────────                                       ─────────────────
Env_Task (200ms)                                       Sync_Task
  | AHT20+BMP280 읽기 (I2C0)                              | k_msgq_get(K_MSEC(500))
  | 상태 변화? -> mbox_send_dt(ENV) ---------- mbox --->  | 태그로 분기:
  |                                                       |   ENV     -> TFT 갱신
Motion_Task (100ms)                                       |   MOTION  -> TFT 갱신
  | MC3419 읽기 (I2C1)                                    |   HEARTBEAT -> WD_OK 전이,
  | 상태 변화? -> mbox_send_dt(MOTION) -------- mbox --->  |                마지막 수신시각 갱신
  |                                                       |   STATUS  -> TFT 갱신
Heartbeat_Task (1000ms, 무조건)                            | 타임아웃(500ms)?
  | mbox_send_dt(HEARTBEAT) -------------------- mbox --->|   마지막 하트비트 후 >= 4000ms?
  |                                                       |     -> WD_TIMEOUT, TRIPS++
Uart_Cmd_Task (폴링)                                       |
  | "1 N"/"2 N"/"3" -> M4 로컬 처리                          rx_cb() -> k_msgq_put(msgq)
  | "4" -> mbox_send_dt(STATUS) ---------------- mbox --->  (ISR-safe, 처리는 전부 Sync_Task에서)
  |   (전부 tx_lock으로 직렬화)
```

## 핀 연결 (이 랩에서 실제로 사용하는 전체 배선 — Lab 09~15와 동일, 새 배선 없음)

### AHT20 + BMP280 (M4, I2C0)

| 모듈 핀 | 연결 대상 | 비고 |
|---|---|---|
| VCC | 3.3V | |
| GND | GND | |
| SDA | SoC GPIO3 (I2C0 SDA) | `i2c0_ms_sda`, 내부 풀업 활성화 |
| SCL | SoC GPIO4 (I2C0 SCL) | `i2c0_ms_scl`, 내부 풀업 활성화 |

### 온보드 가속도계 MC3419 (M4, I2C1)

M4/M55가 물리적으로 공유하는 온보드 `&i2c1` 버스에 이미 실장되어 있는 부품으로, 별도 배선이 필요 없습니다. M4 오버레이에서 `&mc3479 { status = "okay"; };`로 활성화하고, M55 오버레이에서는 `&i2c1`/`&gpio_exp0`/`&ov02c10`을 전부 `disabled`로 꺼서 버스 소유권 충돌을 막습니다(Lab 01부터 이어진 규칙).

### ST7789V3 TFT (M55, SPI0)

| 패널 핀 | 연결 대상 | 비고 |
|---|---|---|
| VCC | 3.3V | |
| GND | GND | |
| SCL/CLK | SPI0 CLK | |
| SDA/MOSI | SPI0 MOSI | |
| CS | SPI0 CS | |
| RES/RST | SoC GPIO17 (J24 3번 핀) | **레벨시프터 필수** |
| DC | SoC GPIO18 (J24 4번 핀) | 위와 동일하게 레벨시프터 경유 |
| BLK(백라이트) | 3.3V 또는 별도 GPIO | 고정 3.3V 연결도 가능 |

**주의**: SPI0을 켜면 M55의 유일한 UART(UART1, GPIO23/24)를 꺼야 합니다. J14 USB-C 커넥터와 J25 헤더는 서로 다른 두 UART가 아니라, **물리적으로 동일한 UART1 신호**를 온보드 USB-시리얼 브리지(J14)와 외부 컨버터용 헤더(J25)라는 서로 다른 경로로 꺼내는 것뿐입니다. 그래서 UART1을 끄면 J14로도 J25로도 M55의 로그를 볼 수 없습니다 — 이 랩부터 M55는 시리얼 콘솔을 아예 포기하고, **TFT 화면이 M55 쪽 상태를 확인하는 유일한 수단**입니다(Lab 11에서 확립된 설계, 아래 참고 절). 이 랩의 `lab/prj.conf`에 이미 `CONFIG_UART_CONSOLE=n`, `CONFIG_CONSOLE=n`이 설정되어 있고, M55 코드에 남아있는 `LOG_INF`/`printk` 호출은 디버깅 편의를 위해 그대로 둔 것일 뿐 실제로는 어디로도 출력되지 않습니다.

### 조작자 커맨드 입력 (M4 콘솔 UART, 새 배선 없음)

| 대상 | 연결 |
|---|---|
| M4 콘솔 (USB-to-TTL 컨버터) | J24 헤더 13번 핀 = M4 TX, 14번 핀 = M4 RX, GND 공통 (컨버터 TX↔보드 RX, 컨버터 RX↔보드 TX 교차 연결) — 230400bps, 8N1 |

## devicetree 설정

M4 오버레이는 Lab 12/13/15와 동일한 I2C0 설정(AHT20+BMP280용)에, Lab 09/10과 동일한 `&mc3479 { status = "okay"; };`(가속도계용)를 함께 둡니다. M55 오버레이는 Lab 11~15와 동일한 TFT/SPI0 설정과 `&i2c1`/`&gpio_exp0`/`&ov02c10` disable을 그대로 유지합니다.

## prj.conf

- `lab/remote/prj.conf` (M4): `CONFIG_I2C=y`(AHT20/BMP280 raw I2C용) + `CONFIG_SENSOR=y`/`CONFIG_MC3419=y`(가속도계 센서 서브시스템용) + UART 폴링 커맨드(추가 Kconfig 불필요).
- `lab/prj.conf` (M55): SPI/GPIO 활성화, M55 콘솔 UART 비활성화 — Lab 11~15와 동일.

## 빌드 방법

west 워크스페이스 루트(`zephyr/`가 보이는 디렉터리)에서 실행합니다. **SR110은 M4 이미지를 먼저 빌드하고, M55를 빌드할 때 그 M4 바이너리를 `M4_BUILD`로 가져와 함께 패키징하는 순서를 반드시 지켜야 합니다** — Lab 12부터 확립된 규칙이며, 이 순서를 지키지 않으면 최종 flash 이미지에 M4 펌웨어가 아예 포함되지 않습니다.

```bash
# 1) M4 (remote) 이미지 먼저 빌드
west build -p always -b sr100_rdk/sr100/m4 ./zephyr_ipc_sr110/16_capstone_gateway/lab/remote -d m4

# 2) M55 (host) 이미지 빌드 -- M4_BUILD로 위에서 만든 M4 바이너리를 가져와 함께 패키징
west build -p always -b sr100_rdk/sr100/m55 ./zephyr_ipc_sr110/16_capstone_gateway/lab -d m55 \
    -DCONFIG_SR100_RELEASE_M4_RESET=y -DM4_BUILD="../m4"
```

`M4_BUILD`는 M55 빌드 디렉터리(`-d m55`) 기준 상대 경로입니다. `m4/`, `m55/`가 워크스페이스 루트 아래 형제 디렉터리인 배치라면 `../m4`가 맞습니다.

## 실행 및 결과 확인

1. M4 콘솔(J24, 230400bps)을 열어둡니다. M55는 이 랩부터 시리얼 콘솔이 없으므로(위 핀 연결 절의 주의 참고) 별도로 열 M55 콘솔은 없습니다 — M55 쪽 상태는 TFT 화면으로 확인합니다.
2. 두 코어 모두 플래시 후 리셋하면, M4는 부팅 직후 가속도계 베이스라인 캘리브레이션(~1초, "가만히 두세요" 로그)을 수행합니다. M55의 TFT에는 제목과 함께 ENV/MOTION 배지가 회색("ENV --"/"MOTION --"), 워치독이 회색 "WAIT"로 표시됩니다.
3. 약 1초 안에 M4가 첫 ENV 이벤트(최초 상태)와 첫 하트비트를 보내면서 TFT가 채워집니다 — ENV 배지가 초록 "ENV OK"(또는 빨강 "ENV HIGH"), 워치독이 초록 "OK"로 바뀝니다.
4. M4 콘솔에 `3`을 입력해 현재 M4의 전체 상태를 로컬로 확인합니다.
5. 손으로 센서를 감싸 쥐거나 따뜻한 물체를 가까이 대서 온도를 임계값(기본 28C) 위로 올려봅니다 — TFT의 ENV 배지가 즉시 빨강 "ENV HIGH"로 바뀝니다.
6. 보드를 흔들거나 기울여봅니다 — TFT의 MOTION 배지가 빨강 "MOTION!"으로 바뀌고 잠시 후(진동이 멈추면) 다시 초록 "MOTION OK"로 돌아옵니다.
7. M4 콘솔에 `4`를 입력합니다 — M4 콘솔에 `STATUS pushed to M55`가 출력되고, TFT 하단의 STATUS 줄이 `STATUS:#1 up=NNs` 형태로 갱신됩니다. `4`를 다시 입력하면 카운터가 `#2`로 올라갑니다.
8. M4 콘솔에서 `1 <celsius>`/`2 <milli-g>`로 임계값을 바꿔보고, ENV/MOTION 배지가 새 임계값 기준으로 반응하는지 확인합니다.
9. **워치독 동작 확인**: M4 콘솔에 `5 5`를 입력해 `Heartbeat_Task`를 5초간 일시정지시킵니다(M4와 M55는 한 칩 안의 두 코어일 뿐이라 M4만 따로 전원을 끊거나 리셋할 수 없으므로, Lab 14와 동일하게 송신 스레드만 멈추는 방식으로 하트비트 끊김을 재현합니다). 4초(`WATCHDOG_TIMEOUT_MS`) 후 TFT 워치독 배지가 빨강 "TIMEOUT"으로 바뀌고 TRIPS 카운트가 1 증가하는지 확인하세요. 5초가 지나면 하트비트가 자동으로 재개되어 배지가 다시 초록 "OK"로 돌아옵니다. `6`을 입력하면 일시정지를 즉시 취소하고 곧바로 재개할 수도 있습니다.

## 정리

Lab 16은 이 커리큘럼에서 확립된 거의 모든 패턴 — raw I2C/SPI 드라이버, 다중 센서 폴백과 이중 장애 지표, 능동 복구, 상태-변화-시-전송, 유한 타임아웃 워치독, 커맨드별 M4-로컬/M55-릴레이 판단 — 을 새 하드웨어 없이 하나의 시스템으로 조합했습니다. 특히 "완전한 저전력 대기(Lab 15)"와 "주기적 안전 감시(Lab 14)"가 한 시스템 안에서 공존할 수 없는 것은 아니지만, 공존하려면 반드시 어느 정도의 깨어남 빈도를 대가로 치러야 한다는 점을 코드로 직접 확인했습니다.

**실기 검증 완료.** ENV/MOTION 배지가 임계값 통과에 맞춰 색이 바뀌는 것, 커맨드 `1`/`2`/`3`(M4-로컬)과 `4`(M55로 릴레이되는 STATUS push)가 모두 정상 동작하는 것, 그리고 M55는 SPI0(TFT)을 쓰는 순간부터 시리얼 콘솔이 아예 없다는 점(자세한 경위는 트러블슈팅 문서 참고)까지 — M4 콘솔 하나만 열어둔 상태로 전체 흐름을 확인했습니다.

## 참고

- Lab 15 (`15_low_power_sync`) — 이 랩의 ENV/MOTION 상태-변화-시-전송 원칙과 `K_FOREVER` vs 유한 타임아웃 대비의 원본.
- Lab 14 (`14_heartbeat_watchdog`) — 이 랩의 하트비트/워치독 상태 머신(`WD_WAITING`/`WD_OK`/`WD_TIMEOUT`)의 원본.
- Lab 13 (`13_uart_bridge`) — 이 랩이 재사용하는 `uart_poll_in()` 폴링 커맨드 입력 패턴과, 커맨드별 M4-로컬/M55-릴레이 판단 원칙의 원본.
- Lab 12 (`12_aht20_bmp280_logging`) — 이 랩의 AHT20+BMP280 raw I2C 드라이버, 다중 센서 폴백, 이중 장애 지표, 능동 복구 로직의 원본.
- Lab 09/10 (`09_accel_telemetry`, `10_threshold_event`) — 이 랩의 가속도계 베이스라인 캘리브레이션과 임계값 이벤트 로직의 원본.
- Lab 11 (`11_tft_sensor_display`) — 이 랩이 그대로 재사용하는 ST7789V3 raw-SPI 드라이버의 원본 및 레벨시프터/SPI0 핀 공유 관련 상세 설명.

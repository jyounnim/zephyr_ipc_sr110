# Lab 11: TFT 센서 디스플레이 (M4 가속도계 → M55 → ST7789V3 컬러 TFT)

M4가 온보드 MC3419 가속도계를 주기적으로 읽어 M55로 보내고, M55는 그 값을 SPI로 연결된
1.69인치 240x280 ST7789V3 컬러 TFT 패널에 실시간으로 표시하는 랩입니다. Lab 01~10이
전부 "값을 콘솔 로그로 확인"하는 구조였다면, 이 랩은 처음으로 **M55가 자신의 시리얼
콘솔을 포기하고, 그 대신 물리 디스플레이를 출력 수단으로 쓰는** 구조를 다룹니다.

## 학습 목표

- 하나의 물리 핀이 여러 주변장치(SPI, UART, I2C 등)로 멀티플렉스되는 SoC에서, **한쪽
  기능을 켜면 다른 쪽 기능을 반드시 포기해야 하는 하드웨어 제약**을 실제로 마주치고
  해결 방향을 결정하는 경험을 쌓는다.
- Zephyr의 in-tree 디스플레이 드라이버(Display/CFB 서브시스템)를 쓰지 않고, **자체 정의
  devicetree 바인딩 + `spi_write_dt()` 직접 호출**로 디스플레이 컨트롤러를 구동하는
  "raw SPI 드라이버" 작성 패턴을 이해한다.
- SPI 컨트롤러의 하드웨어 제약(FIFO 깊이)이 소프트웨어 설계(전송 청크 크기)에 어떻게
  반영되어야 하는지 이해한다.
- Lab 09에서 확립한 "센서 텔레메트리 → mbox → ISR/큐/워커스레드" 패턴을, 수신 측 처리
  내용만 "로그 출력"에서 "디스플레이 렌더링"으로 바꿔 그대로 재사용한다.

## 이전 랩과의 연결

Lab 09는 M4가 가속도계를 읽어 M55로 보내고, M55는 그 값을 **콘솔 로그**로만 출력했습니다.
이 랩은 M4 쪽 센서 읽기 로직은 Lab 09와 완전히 동일하게 재사용하고, M55 쪽만 "로그 출력"
대신 "TFT에 그리기"로 바꿉니다. 즉 IPC 프로토콜과 M4 코드는 Lab 09의 재탕이고, 이 랩의
진짜 새로운 내용은 전부 M55가 디스플레이를 구동하는 부분에 있습니다.

원래 커리큘럼 초안에서 Lab 11은 M55→M4→SSD1306(I2C OLED) 구조였으나, 실제 확보한 패널이
I2C가 아니라 SPI 기반이었고, 이후 사용자가 센서(M4)/디스플레이(M55) 역할 분담과 패널
자체(SSD1306 → ST7789V3 컬러 TFT)를 모두 바꾸기로 결정해 지금의 구조가 되었습니다.

## 핵심 IPC/하드웨어 개념

### 1. 핀 멀티플렉싱과 "기능 하나를 켜면 다른 하나를 포기해야 하는" 제약

이 SoC의 GPIO 핀 상당수는 여러 주변장치 기능이 하나의 물리 패드에 멀티플렉스되어
있습니다. 이 랩에서 마주치는 것은 다음 조합입니다.

| 물리 핀 | 겹치는 기능들 |
|---|---|
| SR110_GPIO23 | SPI0(SPI_MSTR) MOSI / M55 콘솔(UART1) TX |
| SR110_GPIO24 | SPI0(SPI_MSTR) MISO / M55 콘솔(UART1) RX |

M55가 TFT를 SPI0로 구동하려면 이 두 핀을 SPI0 용도로 pinctrl을 잡아야 하고, 그 순간
`ns16550_uart1`(M55의 콘솔)은 물리적으로 더 이상 정상 동작할 수 없습니다. 이런 상황에서
선택지는 대략 두 가지입니다.

1. 콘솔을 다른 물리 UART/핀 그룹으로 옮긴다 (이 SoC는 M4가 이미 UART0을 콘솔로 쓰고
   있어서, M55가 UART0을 또 쓰면 이번엔 M4/M55가 UART0을 두고 충돌합니다 — 이 커리큘럼
   구조에서는 선택지가 아닙니다).
2. 콘솔을 아예 포기하고, 필요한 정보를 **다른 출력 수단**(이 랩에서는 TFT 화면)으로
   대체한다.

이 랩은 2번을 택했습니다 — `Display_Task`가 받은 값을 화면에 그대로 찍는 것이 곧 이
랩의 "콘솔 로그"를 대신하는 셈입니다.

### 2. Zephyr in-tree 디스플레이 드라이버 대신 raw SPI를 쓰는 이유

Zephyr는 `"sitronix,st7789v"`라는 표준 in-tree 디스플레이 드라이버를 제공하고,
`CONFIG_DISPLAY` + `CONFIG_CHARACTER_FRAMEBUFFER`(cfb)와 조합해 쓰는 것이 일반적인
정석 경로입니다. 이 랩도 처음엔 이 경로로 시도했지만, 이 프로젝트가 쓰는 Zephyr
버전(4.4.1)의 해당 바인딩이 요구하는 `mipi-mode` devicetree 프로퍼티 설정이 계속
빌드 실패로 이어졌습니다(자세한 내용은 이 랩의 트러블슈팅 기록 참고).

대신, 같은 보드(SR110)에서 이미 하드웨어까지 검증된 별도 프로젝트
(`jyounnim/zephyr_display-sr110`의 `06_TFT_ST7789V3` 랩)의 방식을 그대로 이식했습니다
— Zephyr in-tree 드라이버를 아예 쓰지 않고, **자체 정의한 최소한의 devicetree
바인딩(`zds,st7789v`)** 으로 배선 정보(SPI 버스/CS, RESET/DC 핀, 패널 크기)만 담아두고,
애플리케이션 코드가 `spi_write_dt()`로 ST7789 커맨드 프로토콜을 직접 구현하는 방식입니다.

이 방식의 장단점:

- **장점**: Zephyr 버전에 따라 계속 바뀌는 in-tree 드라이버의 devicetree 요구사항에
  얽매이지 않는다. 커맨드 시퀀스를 코드에서 직접 눈으로 보고 이해할 수 있다.
- **단점**: cfb 같은 표준 프레임버퍼 API(자동 줄바꿈, 폰트 관리 등)를 못 쓰므로, 사각형
  채우기/글자 그리기 같은 기본 기능을 직접 구현해야 한다 (이 랩의 `st7789_fill_rect()`,
  `st7789_draw_char()`가 그 역할).

### 3. SPI 하드웨어 FIFO 깊이와 전송 청크 크기

SR110의 SPI0 하드웨어 FIFO는 8바이트밖에 안 됩니다. `spi_write_dt()` 한 번에 8바이트를
넘는 데이터를 보내려고 하면, 중간에 FIFO를 다시 채워야 하는 리필 인터럽트가 발생해야
하는데 이게 정상적으로 처리되지 않아 `-ETIMEDOUT`으로 실패합니다. 그래서 이 랩의
`st7789_send()`는 모든 전송을 **8바이트 단위로 쪼개서** 보냅니다.

```c
#define ST7789_CHUNK_BYTES 8

static int st7789_send(int dc_value, const uint8_t *data, size_t len)
{
	gpio_pin_set_dt(&dc_spec, dc_value);
	while (len) {
		size_t chunk = MIN(len, ST7789_CHUNK_BYTES);
		/* ... spi_write_dt() one chunk at a time ... */
		data += chunk;
		len -= chunk;
	}
	return 0;
}
```

이는 SR110이라는 이 특정 SoC의 SPI 컨트롤러 제약이며, 다른 보드로 코드를 옮길 때는
그 보드의 SPI FIFO 크기에 맞게 이 값을 다시 확인해야 합니다.

## 아키텍처

```
M4 (CLIENT)                              M55 (HOST)
─────────────                            ─────────────
mc3479 노드 활성화(overlay)               SPI0 + zds,st7789v 오버레이
device_is_ready()                        ns16550_uart1 비활성화(콘솔 포기)
sensor_attr_set(ODR)      ─┐
sensor_attr_set(FULL_SCALE)│  초기화(1회)   부팅 시 1회:
                            ┘               st7789_reset()
루프(500ms마다):                            st7789_init() (풀 파워/감마 시퀀스)
  sensor_sample_fetch()                     TFT에 "LAB11 ACCEL" 제목 표시
  sensor_channel_get() x3 (X/Y/Z)
  msg = {x, y, z, seq++}
  mbox_send_dt(&tx_channel, &msg) ──mbox──▶ rx_cb(ISR)
                                             k_msgq_put(K_NO_WAIT)
                                                    │
                                             Display_Task(워커 스레드)
                                               k_msgq_get()
                                               st7789_fill_rect()/draw_string()
                                               으로 SEQ/X/Y/Z를 화면에 갱신
```

- **M4 (`lab/remote/src/main.c`, CLIENT)**: Lab 09와 동일. `mc3479` 노드를 열고, 부팅
  시 ODR/측정범위를 설정한 뒤 500ms 주기로 X/Y/Z를 읽어 `struct ipc11_accel_msg`에 담아
  전송합니다.
- **M55 (`lab/src/main.c`, HOST)**: `rx_cb()`는 메시지를 큐에 넣기만 하고 즉시 반환합니다
  (SPI로 TFT를 그리는 작업은 시간이 걸리는 블로킹성 작업이라 ISR에서 직접 할 수 없습니다
  — Lab 03/07에서 확립된 규칙). 별도의 `Display_Task` 워커 스레드가 큐에서 값을 꺼내
  화면에 그립니다.

## 핀 연결 (하드웨어 배선)

M4가 읽는 MC3419 가속도계는 **보드에 이미 내장**되어 있어 별도의 외부 배선이 필요
없습니다(Lab 09와 동일). 이 랩에서 새로 배선해야 하는 것은 M55에 연결하는 **외부 ST7789V3
TFT 패널**뿐입니다.

### ⚠️ 레벨 시프터가 반드시 필요합니다

SR110의 SPI0/GPIO 패드는 **1.8V I/O**로 동작합니다. 일반적으로 유통되는 ST7789V3 모듈의
IOVCC는 3.3V이고, 이 모듈이 로직 하이로 인식하는 최소 전압(VIH ≈ 0.7 × IOVCC ≈ 2.31V)을
1.8V 신호는 넘지 못합니다. 레벨 시프터 없이 직결하면 **SPI 통신 자체는 에러 없이 끝나는데
화면은 계속 검게만 나오는** 증상이 나타납니다(SPI 버스 동작은 정상이지만 패널이 로직
레벨을 유효한 값으로 인식하지 못하는 상태). 아래 5개 신호 전부 양방향 레벨 시프터를
거쳐야 합니다.

| 신호 | 방향 | 비고 |
|---|---|---|
| SCLK | M55 → 패널 | 계속 토글되는 클럭 신호 |
| MOSI | M55 → 패널 | |
| CS | M55 → 패널 | 하드웨어 네이티브 CS (별도 GPIO 아님) |
| RST | M55 → 패널 | |
| DC | M55 → 패널 | |

검증된 레벨 시프터 모듈: **TXS0108E** (자동 방향 감지형). 다만 TXS0108E는 SCLK처럼 빠르게
계속 토글하는 단방향 신호에는 완벽하게 맞는 부품이 아니어서(대략 1~2MHz 이상에서
배선/기생 커패시턴스에 따라 불안정할 수 있음), 이 랩은 `spi-max-frequency`를 4MHz로
보수적으로 설정했습니다. 더 정확한 선택은 74LVC245/74AHCT125 같은 전용 단방향 버퍼입니다.
MISO는 이 패널이 쓰기 전용(MCU가 패널로부터 데이터를 읽어올 필요가 없음)이라 배선하지
않아도 됩니다.

### 핀 매핑

| 신호 | SoC 핀 | 헤더 위치 | devicetree 프로퍼티 |
|---|---|---|---|
| SPI0 CLK | SR110_GPIO22 | J25 (Left 20pin) | `spi_mstr_clk` pinctrl |
| SPI0 MOSI | SR110_GPIO23 | J25 (Left 20pin) | `spi_mstr_mosi` pinctrl (M55 UART1 TX와 공유) |
| SPI0 MISO | SR110_GPIO24 | J25 (Left 20pin) | `spi_mstr_miso` pinctrl (배선 불필요, M55 UART1 RX와 공유) |
| SPI0 CS | SR110_GPIO21 | J25 (Left 20pin) | `spi_mstr_cs` pinctrl (하드웨어 네이티브 CS) |
| RESET | SR110_GPIO17 | J24, 3번 핀 | `reset-gpios = <&gpioa 17 GPIO_ACTIVE_LOW>` |
| DC | SR110_GPIO18 | J24, 4번 핀 | `dc-gpios = <&gpioa 18 GPIO_ACTIVE_HIGH>` |
| VCC | 3.3V (패널 쪽 레벨 시프터 HV 레일) | — | — |
| GND | 공통 | — | — |

RESET/DC는 J24(M4 콘솔과 같은 헤더)의 3/4번 핀을 쓰지만 M4 콘솔(13/14번 핀)과는 물리적으로
다른 핀이라 충돌하지 않습니다.

## devicetree 설정

### M4 오버레이 (`lab/remote/boards/sr100_rdk_sr100_m4.overlay`)

Lab 09와 동일하게 온보드 가속도계만 재활성화합니다.

```dts
&ipc0 {
	shared-memory-size = <0x400>;
};

&mc3479 {
	status = "okay";
};
```

### M55 오버레이 (`lab/boards/sr100_rdk_sr100_m55.overlay`)

I2C1(M4가 소유) 비활성화는 이 커리큘럼 전 랩 공통 패턴이고, 이 랩에서 새로 추가된
부분은 SPI0/TFT 설정과 콘솔(UART1) 비활성화입니다.

```dts
&i2c1 { status = "disabled"; };
&gpio_exp0 { status = "disabled"; };
&ov02c10 { status = "disabled"; };

/* SPI0가 콘솔(UART1) 핀을 가져가므로 명시적으로 비활성화 */
&ns16550_uart1 {
	status = "disabled";
};

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

`zds,st7789v`는 Zephyr 표준 바인딩이 아니라 이 랩 자체에서 정의한 커스텀 바인딩입니다
(`lab/dts/bindings/display/zds,st7789v.yaml`). 이 파일을 devicetree 컴파일러가 찾을 수
있도록, `lab/CMakeLists.txt`에서 `find_package(Zephyr...)`보다 먼저 `DTS_ROOT`에 이
앱의 경로를 추가해줘야 합니다.

```cmake
list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
```

### prj.conf

M4(`lab/remote/prj.conf`)는 Lab 09와 동일합니다. M55(`lab/prj.conf`)는 in-tree
디스플레이 드라이버를 쓰지 않으므로 `CONFIG_DISPLAY`/`CONFIG_CHARACTER_FRAMEBUFFER`/
`CONFIG_ST7789V` 없이 SPI/GPIO만 있으면 됩니다.

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

`CONFIG_UART_CONSOLE=n`/`CONFIG_CONSOLE=n`이 필요한 이유: `ns16550_uart1`을 devicetree에서
비활성화했는데 보드 defconfig가 기본으로 켜는 `CONFIG_UART_CONSOLE=y`이 남아있으면
`SERIAL_HAS_DRIVER=n`과 충돌해 Kconfig 단계에서 빌드가 아예 실패합니다. (반대로
`CONFIG_SERIAL=n`을 직접 끄면 안 됩니다 — 이 SoC의 Kconfig가 `SOC_SR100_M55`에서
`SERIAL_SUPPORT_ASYNC`를 무조건 y-select하기 때문에 `SERIAL=n`과 충돌해 또 다른 방식으로
빌드가 깨집니다.)

## 빌드 방법

west 워크스페이스 루트(`zephyr/`가 보이는 디렉터리)에서 실행합니다.

```bash
# 1) M4(remote) 먼저 빌드
west build -p always -b sr100_rdk/sr100/m4 ./zephyr_ipc_sr110/11_tft_sensor_display/lab/remote -d m4

# 2) M55(host, M4 바이너리를 M4_BUILD로 포함) 빌드
west build -p always -b sr100_rdk/sr100/m55 ./zephyr_ipc_sr110/11_tft_sensor_display/lab -d m55 \
    -DCONFIG_SR100_RELEASE_M4_RESET=y -DM4_BUILD="../m4"
```

`M4_BUILD`는 M55 빌드 디렉터리(`-d m55`) 기준 상대 경로입니다. `m4/`, `m55/`가 워크스페이스
루트 아래 형제 디렉터리인 배치라면 `../m4`가 맞습니다.

## 실행 및 결과 확인

이 랩은 M55 콘솔을 못 쓰므로(위 "핀 멀티플렉싱" 절 참고), **M4 콘솔만** 외부
USB-to-TTL로 연결해 확인합니다(J24 13/14번 핀, 230400bps — 자세한 배선은
[00_course_overview_kr.md](../../00_course_overview_kr.md#m4--m55-시리얼-콘솔-연결-방법)
참고). M4 콘솔에는 Lab 09와 동일하게 아무 로그도 안 뜨는 게 정상입니다(M4는 이 랩에서도
그냥 계속 값을 보내기만 함).

TFT 화면에는 부팅 직후 "LAB11 ACCEL" 제목이 노란색으로 표시되고, 이후 M4로부터 값을 받을
때마다 아래 형태로 갱신됩니다.

```
LAB11 ACCEL
SEQ:12 RX:12
X:-152        (빨간색)
Y:203         (초록색)
Z:986         (노란색)
```

보드를 움직이거나 기울이면 값이 바뀌는 것을 화면에서 바로 확인할 수 있습니다. (Lab 09와
마찬가지로 캘리브레이션되지 않은 원시값이라, 정지 상태에서도 중력가속도 때문에 0이
아닌 것이 정상입니다.)

## 정리

- 한 물리 핀을 여러 주변장치가 나눠 쓰는 SoC에서는, 원하는 기능을 켜는 대가로 다른 기능
  (특히 디버그 콘솔)을 포기해야 할 수 있습니다 — 이런 경우 "포기한 콘솔을 대신할 출력
  수단"을 설계에 포함시키는 것이 이 랩의 핵심 교훈입니다.
- Zephyr in-tree 드라이버가 버전에 따라 요구사항이 달라 발목을 잡을 때는, 필요한 기능만
  담은 최소 커스텀 devicetree 바인딩 + 직접 SPI 드라이버 작성이 실용적인 대안이 될 수
  있습니다.
- SPI 컨트롤러의 하드웨어 FIFO 깊이 같은 저수준 제약은 반드시 애플리케이션 레벨(전송
  청크 크기)에서 함께 고려해야 합니다.
- M4 쪽 IPC/센서 로직 자체는 Lab 09를 그대로 재사용할 수 있다는 것도 확인했습니다 — 새
  랩이라고 항상 모든 걸 새로 짤 필요는 없습니다.

## 실기 검증 완료 (2026-09-08)

이 문서에 기술된 전체 구성(M4 가속도계 → mbox → M55 raw-SPI ST7789V3 렌더링, mbox
콜백→큐→`Display_Task` 워커 스레드 연동 포함)은 실물 보드에서 정상 동작을 확인했습니다.

## 참고 자료 — 이식 원본

이 랩의 디스플레이 쪽(devicetree 바인딩, raw SPI 드라이버, 레벨 시프터/FIFO 노하우)은
같은 보드(SR110)에서 이미 하드웨어까지 검증된 별도 저장소
[`jyounnim/zephyr_display-sr110`](https://github.com/jyounnim/zephyr_display-sr110)의
`06_TFT_ST7789V3` 랩에서 가져왔습니다. 화면 오프셋, MADCTL 색상/회전, 초기화 실패 등
더 자세한 트러블슈팅은 그 저장소의 `06_TFT_ST7789V3_TROUBLESHOOTING_KR.md`를
참고하세요.

---

문제가 발생했다면 → 이 랩의 트러블슈팅 문서(추후 작성 예정) 또는
[Lab 09 트러블슈팅](../09_accel_telemetry/doc/09_accel_telemetry_troubleshooting_kr.md)
(가속도계 관련 문제는 대부분 동일)을 참고하세요.

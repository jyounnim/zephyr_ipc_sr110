# Lab 16: Capstone Gateway — 트러블슈팅

이 문서는 Lab 16을 실기로 가져가기까지 실제로 발생했던 문제와 그 원인/해결을 기록합니다. 강의 문서(`16_capstone_gateway_kr.md`)는 이미 확정된 최종 코드를 설명하지만, 여기서는 그 코드에 도달하기까지 어떤 함정이 있었는지를 다룹니다.

## 1. M55 빌드 에러 — `DT_N_..._P_spi_max_frequency' undeclared`

### 증상

M55 빌드에서 다음과 같은 에러로 실패:

```
.../m55/zephyr/include/generated/zephyr/devicetree_generated.h:14802:37:
error: 'DT_N_S_soc_S_spi_50315000_S_st7789v_0_P_spi_max_frequency' undeclared here (not in a function);
did you mean 'DT_N_S_soc_S_spi_50315000_S_st7789v_0_P_compatible_LEN'?
```

### 원인

두 가지가 겹쳐서 발생했습니다.

1. **커스텀 devicetree 바인딩 파일 누락**: Lab 11부터 매 랩마다 있던 `lab/dts/bindings/display/zds,st7789v.yaml`(TFT의 `compatible = "zds,st7789v"` 노드가 `spi-device.yaml`을 상속해 `spi-max-frequency` 같은 표준 SPI 자식 프로퍼티를 인식하게 해주는 파일)이 이 랩을 새로 작성하면서 통째로 빠져 있었습니다.
2. **`DTS_ROOT` 확장 누락**: M55의 `lab/CMakeLists.txt`에 있어야 할 `list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})`(devicetree 컴파일러가 앱 로컬 바인딩 디렉터리를 찾게 해주는 설정, `find_package(Zephyr...)`보다 먼저 있어야 함)도 함께 빠져 있었습니다.

이 랩의 M55 `CMakeLists.txt`는 옛 Lab 18 스텁(SSD1306 I2C OLED, Zephyr in-tree 디스플레이 드라이버 사용)을 베이스로 재사용했는데, 그 스텁은 커스텀 바인딩이 애초에 필요 없었던 파일이라 이 차이를 놓쳤습니다. 두 가지가 함께 빠지면서, devicetree 컴파일러가 `zds,st7789v` 노드를 표준 프로퍼티 없이 불완전하게 컴파일했고, 그 결과 `spi-max-frequency` 매크로가 생성되지 않아 `SPI_DT_SPEC_GET()` 매크로 확장 과정에서 컴파일 에러로 이어졌습니다.

### 조치

Lab 15에서 두 파일을 그대로 복사:

- `lab/dts/bindings/display/zds,st7789v.yaml` 추가.
- `lab/CMakeLists.txt` 최상단에 `list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})` 추가.

**교훈**: 새 랩이 이전 랩의 커스텀(비-in-tree) devicetree 바인딩을 재사용할 때는, `.c`/`.overlay` 코드뿐 아니라 그 바인딩이 요구하는 `dts/bindings/` 디렉터리와 `CMakeLists.txt`의 `DTS_ROOT` 설정까지 통째로 세트로 옮겨야 합니다. 코드만 보고 "같은 devicetree 노드니 오버레이만 옮기면 된다"고 판단하면 이번처럼 바인딩 파일 자체가 빠지는 실수가 재발할 수 있습니다.

## 2. 문서 오류 — "SPI0을 쓰면 M55 콘솔은 USB-C(J14)로는 여전히 볼 수 있다"는 잘못된 서술

### 증상

문서에 "SPI0을 켜면 M55의 UART1 콘솔(GPIO23/24 공유)을 꺼야 합니다 ... M55 콘솔을 보려면 반드시 USB-C 직결(J14, 115200bps) 방법을 사용하세요"라는 문장이 있었는데, 실제로는 J14로도 아무 로그가 나오지 않습니다.

### 원인

J14(USB-C 직결)와 J25(외부 USB-to-TTL 컨버터 헤더)는 서로 다른 두 개의 UART가 아니라, M55의 **UART1 하나**를 두 가지 물리 경로(온보드 USB-시리얼 브리지 경유 vs 외부 헤더 경유)로 꺼내는 것뿐입니다. 베이스 보드 devicetree의 핀 테이블에도 `SR110_GPIO23`/`GPIO24`가 "SPI0 MOSI/MISO 겸 M55 콘솔(UART1) TX/RX"로 명시되어 있고, 이 SoC에는 M55용 UART가 이것 하나뿐입니다. 따라서 `&ns16550_uart1 { status = "disabled"; };`으로 UART1 자체를 끄면 J14로도 J25로도 M55의 로그를 전혀 볼 수 없습니다.

이 사실 자체는 Lab 11 문서에는 처음부터 정확히 적혀 있었습니다("M55가 자신의 시리얼 콘솔을 포기하고, 그 대신 물리 디스플레이를 출력 수단으로 쓰는 구조"). 그런데 이후 Lab 13 문서를 작성하면서 "실행 및 결과 확인" 절에 "M55 콘솔(J14 USB-C)을 열어둔다"는 문구가 관성적으로 추가되었고, 이 잘못된 문구가 Lab 14/15/16과 커리큘럼 개요 문서까지 그대로 복제되었습니다. 실기 검증 과정에서 TFT 화면만으로 충분해서 M55 콘솔을 실제로 열어볼 필요가 없었기 때문에 이 오류가 그동안 발견되지 않았습니다.

### 조치

`00_course_overview_kr.md`/`_en.md`, `13_uart_bridge`/`14_heartbeat_watchdog`/`15_low_power_sync`/`16_capstone_gateway`의 한/영 문서에서 관련 문구를 전부 "J14와 J25는 물리적으로 같은 UART1 신호이며, UART1을 끄면 둘 다 죽는다 — SPI0 사용 랩에서 M55 상태를 확인하는 유일한 수단은 TFT 화면이다"로 정정했습니다. **코드(devicetree/prj.conf)는 처음부터 옳았고**, 잘못된 것은 오직 "M55 콘솔을 어떻게 열어야 하는지"에 대한 문서 설명뿐이었습니다.

**교훈**: 온보드 USB 브리지와 외부 헤더가 함께 있는 보드에서는, "커넥터 A를 못 쓰면 커넥터 B는 여전히 쓸 수 있다"고 가정하기 전에 그 둘이 정말 별개의 UART 컨트롤러인지부터 devicetree 핀 테이블로 확인해야 합니다. 이번 오류는 그 확인 없이 이전 랩 문서의 문구를 관성적으로 복사하면서 발생했습니다.

### 실기 검증 결과

위 두 가지를 수정한 뒤, M4 콘솔 하나만 연결한 상태로(M55 콘솔은 애초에 존재하지 않으므로 열 필요가 없음) 다음을 확인했습니다.

- ENV 배지가 온도 임계값 통과에 맞춰 초록(OK)/빨강(HIGH)으로 전환.
- MOTION 배지가 보드를 흔들 때 빨강(ACTIVE)으로, 정지 후 다시 초록(QUIET)으로 전환.
- 워치독 배지가 하트비트 수신 중에는 초록(OK) 유지.
- 커맨드 `1 <celsius>`/`2 <milli-g>`/`3`이 M4 콘솔에서만 로컬로 처리됨(mbox 미사용).
- 커맨드 `4`가 M55로 STATUS를 push하고, TFT 하단 STATUS 줄이 갱신됨.

**Lab 16 완전히 검증 완료.**

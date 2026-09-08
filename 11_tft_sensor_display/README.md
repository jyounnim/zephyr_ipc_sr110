# Lab 11: TFT Sensor Display — M4(accel) → M55 → ST7789V3

개발/검증용 짧은 노트입니다.

> **2026-09-07 재설계**: 원래 이 랩은 M55 → M4 → SSD1306(I2C OLED) 구성이었으나,
> 사용 가능한 실물 패널이 I2C가 아니라 **SPI 7핀 SSD1306**임이 확인되면서(→ M4 SPI0/M55
> UART1 핀 충돌 발견), 이후 사용자가 아예 패널 자체를 **ST7789V3 컨트롤러 기반 1.69인치
> 240x280 컬러 SPI TFT**로 교체하기로 결정했습니다. 이와 함께 방향도 뒤집혔습니다:
> **M4가 센서(가속도계, Lab 09와 동일 파이프라인)를 읽고, M55가 그 값을 TFT에 표시**합니다.

> **2026-09-07 추가 변경 — Zephyr in-tree 디스플레이 드라이버 폐기**: 처음엔 Zephyr
> 표준 `"sitronix,st7789v"` 바인딩 + `CONFIG_DISPLAY`/`CONFIG_CHARACTER_FRAMEBUFFER`
> 조합으로 시도했으나, 이 프로젝트가 쓰는 Zephyr 버전(4.4.1)의 해당 바인딩이 요구하는
> `mipi-mode` devicetree 프로퍼티 문제로 빌드가 계속 실패했습니다. 이후 사용자가 이미
> **같은 보드(SR110)에서 하드웨어까지 검증해둔 별도 저장소** `jyounnim/zephyr_display-sr110`의
> `06_TFT_ST7789V3` 랩을 알려주셔서, 그 코드를 그대로 이식했습니다 — Zephyr in-tree 드라이버를
> 아예 안 쓰고 자체 정의한 `"zds,st7789v"` devicetree 바인딩 + `spi_write_dt()` 로 직접
> 패널을 구동하는 방식입니다. `mipi-mode` 문제 자체가 원천적으로 사라지고, SPI0의 8바이트
> FIFO 한계·레벨 시프터 필요 여부 등 이 보드에서 이미 실기로 확인된 노하우까지 함께 가져올 수
> 있어 지금은 이 방식으로 확정했습니다.

## 구성

- `lab/remote/src/main.c` — **CLIENT (M4)**: 온보드 MC3419 가속도계를 500ms 주기로 읽어
  `struct ipc11_accel_msg{x,y,z,seq}`로 M55에 전송 (Lab 09와 동일한 웨이크업 시퀀스 재사용).
- `lab/src/main.c` — **HOST (M55)**: mbox 콜백은 큐잉만 하고, `Display_Task` 워커 스레드가
  `zephyr_display-sr110` Lab 06에서 검증된 raw-SPI ST7789V3 드라이버 함수(`st7789_init`/
  `st7789_reset`/`st7789_fill_rect`/`st7789_draw_string` 등)로 값을 TFT에 표시.
- `lab/dts/bindings/display/zds,st7789v.yaml` — Zephyr in-tree 드라이버 대신 쓰는 자체 정의
  devicetree 바인딩 (`06_TFT_ST7789V3`에서 그대로 복사).

> **참고**: M4/M55는 물리적으로 같은 I2C1 버스를 공유합니다. `lab/boards/sr100_rdk_sr100_m55.overlay`에서
> `&i2c1`을 disable해 M4만 이 버스를 쓰도록 되어 있습니다 — 자세한 원인은
> [Lab 01 README](../01_hello_ipc/README.md#해결된-이슈-2026-08-30) 참고.

## 빌드

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

## ⚠️ 하드웨어 필수 사항 — 레벨 시프터

`zephyr_display-sr110` Lab 06 실기 검증 결과: SR110의 SPI0/GPIO 패드는 **1.8V I/O**인데,
일반적인 ST7789V3 모듈의 IOVCC는 3.3V(VIH ≈ 2.31V)라 1.8V로 직접 구동하면 SPI 에러 하나 없이
**화면이 계속 검게만 나옵니다**. SCLK/MOSI/CS/RST/DC 5개 신호 전부 양방향 레벨 시프터
(검증된 모듈: **TXS0108E**)를 거쳐야 합니다. TXS0108E는 자동 방향 감지 방식이라 SCLK처럼
빠르게 계속 토글되는 단방향 신호엔 완벽하지 않을 수 있어(대략 1~2MHz 이상에서 배선/기생
커패시턴스에 따라 불안정할 수 있음), 이 오버레이는 `spi-max-frequency = <4000000>`로
보수적으로 설정했습니다. 더 정확한 선택은 74LVC245/74AHCT125 같은 전용 단방향 버퍼입니다.

## 참고 문서 — 이식 원본

디스플레이 쪽(devicetree 바인딩, raw SPI 드라이버, 레벨 시프터/FIFO 노하우)은
[`jyounnim/zephyr_display-sr110`](https://github.com/jyounnim/zephyr_display-sr110)의
`06_TFT_ST7789V3` 랩에서 이미 이 보드로 실기 검증된 내용을 그대로 가져왔습니다. 더 자세한
트러블슈팅(화면 오프셋, MADCTL 색상/회전, 초기화 실패 등)은 그 저장소의
`06_TFT_ST7789V3_TROUBLESHOOTING_KR.md`를 참고하세요.

## 실기 검증 완료 (2026-09-08)

M4(가속도계) → mbox → M55(ST7789V3 TFT) 전체 조합, 실물 보드에서 동작 확인 완료. TFT에
"LAB11 ACCEL" 제목과 SEQ/X/Y/Z 값이 정상적으로 갱신되는 것을 확인했습니다. 새로 추가했던
폰트 확장 문자(숫자/`-`/`:`/대문자)와 mbox 콜백→큐→`Display_Task` 워커 스레드 연동 모두
이 랩 조합으로 실기 검증 완료.

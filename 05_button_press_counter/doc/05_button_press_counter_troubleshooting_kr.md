# Lab 05 트러블슈팅: Button Press Counter

Lab 05 진행 중 실제로 발생했던 이슈와 그 원인/조치를 정리합니다. Lab 01, 02와 동일한 계열의 이슈가 재등장하므로, 해당 랩의 원인 분석을 함께 참고하세요.

### 증상: I2C1 버스 충돌 (M4/M55 부팅 실패 또는 gpio_exp0 동작 이상)

M4와 M55는 물리적으로 같은 I2C1 버스(SCL/SDA 핀)를 공유합니다. 두 코어가 동시에 이 버스를 초기화하려고 하면 버스 초기화 충돌이 발생할 수 있습니다.

**원인**: `user_button`(및 LED0/LED1)이 매달려 있는 `gpio_exp0`(PCAL6416A GPIO 확장 칩)는 I2C1 버스 위에 있는데, M4/M55 두 이미지 모두 기본적으로 이 버스를 초기화하려 시도합니다.

**조치**: `lab/boards/sr100_rdk_sr100_m55.overlay`에서 M55 쪽 `&i2c1`을 `status = "disabled"`로 비활성화하여, M4만 이 버스를 실제로 구동하도록 했습니다. 단, 부모 버스만 비활성화하면 자식 노드(`&gpio_exp0`, `&ov02c10`)가 기본 `status = "okay"`를 유지해 `DT_BUS()`가 비활성화된 i2c1 핸들을 찾지 못하는 빌드 에러가 나므로, 자식 노드도 함께 명시적으로 비활성화해야 합니다.

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

자세한 원인은 [Lab 01 트러블슈팅 문서](../../01_hello_ipc/doc/01_hello_ipc_troubleshooting_kr.md) 참고.

---

### 증상: M4 부팅 로그에 `interrupt configuration failed: -134`, 버튼을 눌러도 아무 반응 없음

```
[00:00:00.010,000] <err> gpio_keys: interrupt configuration failed: -134
[00:00:00.010,000] <err> gpio_keys: Pin 0 interrupt configuration failed: -134
```

`-134`는 Zephyr의 `-ENOTSUP`입니다.

**원인**: `user_button`은 `gpio_exp0`(PCAL6416A, I2C GPIO 확장 칩) 뒤에 달려 있는데, M4 쪽 base devicetree의 `gpio_exp0` 정의에는 `int-gpios` 프로퍼티가 없습니다(M55 쪽 정의에는 `int-gpios = <&gpioa 3 GPIO_ACTIVE_LOW>;`가 있지만, M55는 이 랩에서 i2c1을 disable했으므로 무의미합니다). `gpio_pcal64xxa.c` 드라이버는 `int-gpios`가 없는 인스턴스에 대해 `pin_interrupt_configure()`가 항상 `-ENOTSUP`을 반환하도록 되어 있어서, 기본 인터럽트 기반 `gpio-keys` 모드는 이 보드의 M4 쪽에서 원천적으로 동작할 수 없습니다. Lab 02에서 처음 발견된 것과 동일한 이슈입니다.

**조치**: `zephyr/dts/bindings/input/gpio-keys.yaml`에 정확히 이 상황을 위한 `polling-mode` 불리언 프로퍼티가 있습니다. M4 오버레이(`lab/remote/boards/sr100_rdk_sr100_m4.overlay`)에 다음을 추가해 인터럽트 대신 주기적 폴링(기본 `debounce-interval-ms` 30ms)으로 전환했습니다.

```dts
&buttons {
	polling-mode;
};
```

**주의**: `polling-mode`는 반드시 `gpio-keys` **부모 노드**(`&buttons`)에 적용해야 합니다. 개별 버튼 자식 노드(`&user_button`)에 붙이면 devicetree 바인딩 검증 에러가 발생합니다.

---

### 증상: `M4_BUILD` 경로를 잘못 지정해서 M55 빌드가 M4 이미지를 못 찾음

**원인**: `M4_BUILD`는 M55 빌드 디렉토리(`m55/`) 기준 **상대 경로**입니다. 절대 경로나 다른 기준으로 착각하면 M55 빌드 시 M4 바이너리를 찾지 못합니다.

**조치**: `m4/`와 `m55/`가 워크스페이스 루트의 형제 디렉토리 구조라면 `-DM4_BUILD="../m4"`가 맞습니다. 자세한 원인은 [Lab 01 트러블슈팅 문서](../../01_hello_ipc/doc/01_hello_ipc_troubleshooting_kr.md) 참고.

```bash
west build -p always -b sr100_rdk/sr100/m55 ./zephyr_ipc_sr110/05_button_press_counter/lab -d m55 \
    -DCONFIG_SR100_RELEASE_M4_RESET=y -DM4_BUILD="../m4"
```

---

## 설계 검토 — mbox 콜백 블로킹 금지 원칙 (문제 없음으로 확인됨)

Lab 03에서 확립된 원칙("mbox 콜백에서 블로킹 호출 금지")에 비추어 이 랩의 구조를 점검한 결과, 별도 조치가 필요한 문제는 없었습니다.

- **M55 쪽** `mbox_rx_callback()`은 로그만 남기고 즉시 반환하므로(블로킹 호출 없음) 문제 없습니다.
- **M4 쪽**은 mbox 콜백이 아니라 입력 서브시스템 콜백(`button_input_cb`, 워크큐 컨텍스트)에서 메시지 큐에 넣기만 하고, 실제 mbox 전송은 별도의 `Send_Task` 워커 스레드에서 처리하는 구조라 이미 안전합니다.

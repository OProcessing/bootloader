# STM32F446RE Bootloader 예제

STM32CubeIDE의 `bootFW`와 `AppFW` 프로젝트로 구성합니다. 2단계에서는
공유 Metadata ABI, Flash 저널, 부팅 시도 제한, App 확인 처리를 추가했습니다.
3단계는 IWDG, Reset Cause에 따른 실패 판정, 런타임 실패 누적과 상태 감시를 구현합니다.
Recovery는 Bootloader `while` 대기로 유지하며 업데이트 통신은 추가하지 않습니다.

## 메모리 맵

| 영역 | 주소 범위 | 크기 | Erase 섹터 |
|---|---|---|---|
| Bootloader | `0x08000000–0x08007FFF` | 32 KiB | 0–1 |
| Application | `0x08008000–0x0803FFFF` | 224 KiB | 2–5 |
| Metadata A | `0x08040000–0x0805FFFF` | 128 KiB | 6 |
| Metadata B | `0x08060000–0x0807FFFF` | 128 KiB | 7 |

이전 App 352 KiB / Metadata 128 KiB 배치에서 변경했습니다. 같은 섹터 안의
레코드 두 개는 섹터 삭제 중 전원이 끊기면 함께 사라집니다. 따라서 서로 독립적으로
지울 수 있는 Sector 6, 7을 예약합니다. 링커와 `Shared/boot_state.h`가 같은 배치를 사용합니다.
기존 Metadata와의 자동 마이그레이션은 없습니다. 최초에는 아래 절차로 다시 프로비저닝합니다.

하드웨어 기준: [ST RM0390](https://www.st.com/resource/en/reference_manual/dm00135183.pdf).
Flash 백엔드는 3.3 V 보드용 WORD programming / `FLASH_VOLTAGE_RANGE_3`
(2.7–3.6 V) 설정을 사용합니다.

## 부팅 흐름

1. Bootloader `main()` 시작에서 `RCC->CSR`를 읽고 reset flags를 지웁니다.
2. 두 Metadata 저널에서 commit과 CRC가 유효한 최신 sequence를 찾습니다.
3. 기록된 `fw_size` 범위의 SP, Thumb Reset Vector와 펌웨어 CRC32를 검사합니다.
4. `APP_START`와 증가한 `boot_attempts`를 Flash에 저장하고 IWDG를 시작한 뒤 App으로 점프합니다.
5. App 초기화 후 주 루프가 2초 동안 정상 진행하면 `Boot_Confirm()`이 `APP_OK`, attempts=0을 저장합니다.
6. 확인 전에 리셋되면 다음 실행에서 재시도합니다. 최대 3번 App에 진입하며,
   세 번째 시도도 확인 없이 리셋되면 다음 부팅에 `APP_FAIL`을 기록하고 Bootloader에 머뭅니다.

전원 재인가, BOR, watchdog, software reset 등 모든 미확인 리셋은 시도를 소비합니다.
현재 예제는 주 루프 작업 진행을 감시합니다. 실제 제품에서는 핵심 주변장치의 자체 진단을
완료하고 각 필수 작업의 성공 지점에서 `AppHealth_Progress()`를 호출하도록 확장하세요.
정상 확인 후의 다음 부팅은 attempts=1부터 다시 시작합니다.

## 3단계: 장애 감지와 복구 대기

IWDG는 Bootloader가 App으로 점프하기 전에 시작하므로 Reset Handler와 초기화 중의
무한 루프도 감시합니다. App은 직접 디버깅 진입도 지원하도록 `main()` 시작에서 같은
설정을 다시 적용합니다. `/256`, reload=4095이므로 nominal 32 kHz LSI에서 timeout은
32.768초이며 실제 시간은 LSI 오차에 따라 달라집니다. 시작한 IWDG는 system reset 전까지
끌 수 없습니다. [ST RM0390 IWDG 설명](https://www.st.com/resource/en/reference_manual/dm00135183.pdf)을 기준으로 구현했습니다.
Option Bytes는 기본 software-start watchdog (`WDG_SW=1`)을 전제로 하며 변경하지 않습니다.

App은 SysTick ISR에서 watchdog을 갱신하지 않습니다. 필수 작업이 진행될 때만
주 루프가 갱신하므로 주 루프/작업 정지, Error Handler, HardFault 등에서 갱신이 끊기면
IWDG가 MCU를 리셋합니다. `Shared/app_health.h`의 기본 시간 정책은 다음과 같습니다.

| 정책 | 기본값 | 동작 |
|---|---|---|
| 작업 진행 기한 | 1초 | 필수 작업이 기한을 놓치면 fault latch 후 갱신 중단 |
| 부팅 안정 확인 | 2초 | 모든 필수 작업이 정상 진행한 뒤 Boot Confirm |
| 확인 요청 제한 | 8초 | Health 감시 시작 후 Confirm이 처리되지 않으면 갱신 중단 |
| 런타임 안정 확인 | Confirm 후 30초 | 런타임 실패 횟수 초기화 |

8초 제한은 App의 Health 감시 시작 이후에 적용합니다. 그보다 앞선 Reset Handler나
초기화 정지는 IWDG timeout으로 감지합니다. 성공한 동기 Metadata 작업의 대기 시간은
`AppHealth_MaintenanceComplete()`로 소프트웨어 작업 기한/안정 시간에서 제외합니다.
저장 중에는 IWDG를 갱신하지 않으므로 저장 자체가 멈추면 여전히 리셋됩니다.
fault latch 후에는 작업 진행이 다시 보이더라도 갱신을 재개하지 않습니다.

| 직전 Metadata 상태 | 이번 Reset Cause | 판정 |
|---|---|---|
| APP_START | IWDG/WWDG | 부팅 중 watchdog 실패 |
| APP_START | POR/BOR | 부팅 확인 전 전원 중단 |
| APP_START | 그 외 (software/pin/unknown 등) | 부팅 확인 전 리셋 |
| APP_OK | IWDG/WWDG | Confirm 이후 런타임 watchdog 실패 |
| APP_OK | POR/BOR/software/pin 등 | 런타임 실패를 추가하지 않음 |

POR/BOR와 watchdog flag가 동시에 있으면 전원 이벤트로 우선 분류하고 원본 bitmask도
보존합니다. 초기 등록 후 아직 실행 이력이 없는 이미지는 이전 런타임 실패로 오인하지 않습니다.
이전 APP_START가 있으면 전원/수동 리셋도 기존 정책대로 부팅 시도를 소비합니다.

`boot_attempts`는 Confirm에서 0으로 되돌리지만 `runtime_failures`는 유지합니다.
Confirm 직후 반복해서 watchdog 리셋되는 App은 런타임 실패가 3회 누적되면 차단합니다.
이 횟수는 정상 동작 30초를 채워야 초기화되며 전원 재인가나 수동 리셋만으로 지워지지 않습니다.
부팅 3회 실패도 기존대로 차단합니다. `last_failure`는 마지막 세부 원인을 유지하고
`failure_reason`은 현재 APP_FAIL/재시도의 원인을 나타냅니다.

차단되거나 유효한 이미지/Metadata가 없으면 `boot_recovery_mode=1`로 Bootloader의
`while`에서 대기합니다. 이 루프는 IWDG를 갱신하여 대기를 유지합니다. 새 전송 프로토콜이나
자동 업데이트/재점프는 수행하지 않습니다. 이것이 이 예제의 Recovery 범위입니다.

## Metadata 저장 규칙

`Shared/boot_state.h`의 `BootMetadata`는 64바이트 고정 구조체입니다.

- `boot_state`, `fw_version`, `fw_crc32`, `fw_size`
- `boot_attempts`, `total_boots`
- `runtime_failures`, `last_failure`: 기존 2단계 reserved 두 word를 사용하므로 64바이트 ABI와 CRC 위치 유지
- `reset_flags`: RCC CSR 원본, `reset_causes`: POR/BOR/IWDG/WWDG/software/pin/low-power 비트마스크
- `failure_reason`, `sequence`, 형식 버전, Metadata 자체 CRC32, 마지막 commit word

reset flags는 동시에 여러 개가 설정될 수 있으므로 단일 원인으로 임의 축약하지 않습니다.
`fw_version`은 명시한 uint32 값으로 저장하며 버전 비교나 downgrade 제한은 아직 없습니다.
CRC는 CRC-32/ISO-HDLC (반사 다항식 `0xEDB88320`, init/xorout `0xFFFFFFFF`)이고
Python `zlib.crc32()`와 같습니다. 이미지 CRC와 레코드 CRC는 별개입니다.

레코드를 순차 추가하고 마지막에 commit word를 기록합니다. 쓰기 중 전원이 끊긴
레코드는 무시합니다. 현재 섹터가 차면 **다른 섹터만 지운 후** 새 레코드를 기록하므로,
지우는 도중 또는 기록 중 리셋되어도 이전의 유효한 레코드가 남습니다. 매 부팅마다
섹터를 지우지는 않습니다. sequence wrap도 처리합니다.

유효한 레코드가 없으면 자동으로 새 CRC를 계산해 정상 이미지로 등록하지 않습니다.
`BOOT_EMPTY` 또는 `BOOT_CORRUPT`로 정지합니다. 최초 등록 도중 전원이 끊겨 유효한
레코드가 하나도 남지 않은 경우에도 외부 프로비저닝을 다시 수행해야 합니다.
CRC는 손상 검사용이며 서명이나 펌웨어 인증 기능은 아닙니다.

## 빌드와 최초 Flash 기록

STM32CubeIDE에서는 두 프로젝트를 Refresh한 뒤 빌드합니다. 새 파일
`Core/Src/boot_state_port.c`가 빌드 대상에 들어갔는지 확인하세요. 이 파일이
`Shared`의 동일 소스를 포함하므로 두 프로젝트에 별도 복사본을 관리하지 않습니다.
`Shared` 디렉터리도 프로젝트와 같은 상위 폴더에 유지해야 합니다.

IDE의 자동 생성 Makefile을 수정하지 않는 별도 빌드 도구도 제공합니다.
프로젝트 루트의 PowerShell에서 실행합니다. GNU Arm GCC가 PATH에 없으면
`C:/ST`의 STM32CubeIDE 설치를 탐색하며, `--toolchain <tools/bin>`으로 지정할 수도 있습니다.

```powershell
python tools/build_firmware.py
python tools/provision_metadata.py build/AppFW/AppFW.bin --fw-version 0x00010000
```

처음 설치할 때 STM32CubeProgrammer 등에서 다음 순서로 기록합니다.

1. Sector 0–7을 삭제합니다. **이 전체 삭제는 최초 설치용이며 기존 상태를 초기화합니다.**
2. `build/bootFW/bootFW.bin` → `0x08000000`
3. `build/AppFW/AppFW.bin` → `0x08008000`
4. `build/provision/metadata_a.bin` → `0x08040000`
5. `build/provision/metadata_b.bin` → `0x08060000`
6. 모든 쓰기가 끝난 다음 reset/run 합니다.

두 초기 레코드는 같은 이미지 정보를 가지며 sequence는 A=1, B=0입니다.
Metadata 생성에 사용한 **동일한 App .bin**을 기록해야 합니다. App 재빌드/변경 후에는
새 CRC와 크기로 Metadata도 다시 생성해야 합니다. 기존 저널 위에 초기 64바이트만
덮어쓰면 안 됩니다. 외부 재등록 시 Sector 6, 7을 모두 지우고 초기 레코드를 기록하세요.
업데이트 시 전체 삭제를 사용하면 Bootloader와 Metadata도 사라집니다.

IDE에서 만든 ELF를 사용한다면 같은 툴체인의 `arm-none-eabi-objcopy -O binary`
명령으로 `.bin`을 추출하여 생성 도구에 전달할 수 있습니다. 별도 빌드의 출력은
`Debug`가 아닌 `build`에 있으므로 서로 다른 빌드 결과를 섞지 마세요.

## API와 관찰할 변수

| API | 용도 |
|---|---|
| `Boot_MetadataRead()` | CRC/commit이 유효한 최신 레코드 읽기 |
| `Boot_MetadataWrite()` | 새 sequence와 CRC를 붙여 append; 저수준 API |
| `Boot_Prepare()` | 이미지 검증 및 APP_START/시도 횟수 저장 |
| `Boot_Confirm()` | App 정상 확인; APP_OK 및 횟수 초기화; 중복 호출은 추가 쓰기 없음 |
| `Boot_RuntimeHealthy()` | APP_OK에서 장시간 정상 동작 확인 후 런타임 실패 횟수만 초기화 |
| `Boot_MarkFailed()` | App 자체 진단 실패 시 APP_FAIL 영구 기록 |
| `Boot_InstallImage(version, size, crc)` | 향후 업데이트 경로에서 새 이미지 검증 후 등록 및 횟수 초기화 |

설치 API는 명시적 복구/업데이트 시에만 사용하며 매 부팅마다 호출하지 않습니다.
이미 APP_FAIL인 이미지는 자동 재시도하지 않습니다. 복구에는 검증된 이미지 재등록이 필요합니다.
새 이미지 기록 자체의 전원 차단 안전성 및 전송 프로토콜은 이 단계의 범위가 아닙니다.

Flash 쓰기 API는 인터럽트가 활성화되고 HAL SysTick이 실행 중인 thread mode에서만
호출합니다. ISR이나 동시 호출은 지원하지 않습니다. 두 프로젝트는 동시에 실행되지 않으며,
Bootloader는 Metadata 쓰기를 끝낸 후 IRQ/SysTick/NVIC를 정리하고 App으로 분기합니다.

디버거에서 Bootloader의 `boot_result`, `boot_metadata`, `boot_reset_flags`,
`boot_reset_causes`, `boot_recovery_mode`, App의 `app_confirm_result`,
`app_runtime_result`, `app_health`, `app_health_action`, `app_main_heartbeat`를 관찰하세요. 정상 실행 결과는
`app_confirm_result == BOOT_SUCCESS`이고, Flash 최신 상태는 `APP_OK`, attempts=0입니다.
Bootloader의 `boot_metadata`는 점프 전 스냅샷이므로 App 확인 후 상태는
`Boot_MetadataRead()`나 Flash의 최신 레코드로 확인해야 합니다.

## 검증

호스트 테스트는 NOR Flash의 1→0 기록 제약을 모사합니다. MinGW GCC 예시:

```powershell
New-Item -ItemType Directory -Path build/tests -Force
gcc -std=c11 -O2 -Wall -Wextra -Werror Shared/boot_state.c tests/test_boot_state.c -o build/tests/test_boot_state.exe
./build/tests/test_boot_state.exe build/AppFW/AppFW.bin build/provision/metadata_a.bin
gcc -std=c11 -O2 -Wall -Wextra -Werror Shared/app_health.c tests/test_app_health.c -o build/tests/test_app_health.exe
./build/tests/test_app_health.exe
```

상태 전이, 3회 재시도, 확인 후 초기화, 이미지/Metadata CRC 오류, reset cause 비트,
sequence wrap, append/섹터 교체/confirm 중 80가지 word 기록 중단과 섹터 삭제 중단을 검사합니다.
실제 ARM App 바이너리와 Python 생성 Metadata를 C 코드로 읽어 boot/confirm하는 호환성도 확인합니다.
3단계 테스트는 시작/런타임 reset 판정, 각각의 실패 임계값, 안정 구간 후 초기화,
런타임 정상 기록 중 16가지 추가 중단, 다중 작업 기한, fault latch, tick wrap,
Flash 작업의 기한 처리를 확인합니다.

실제 보드에서는 초기화 후 각 reset 원인, Metadata 쓰기/섹터 교체 중 전원 차단을 확인합니다.
자동화된 Flash 모사는 실제 저전압 시 Flash 동작을 대체하지 않습니다.

3단계 보드 시험은 새 App을 빌드한 뒤 같은 바이너리로 Metadata를 다시 생성/기록하고 수행합니다.
디버거에서 `app_fault_injection=1`이면 즉시 무한 루프, `=2`이면 주 루프는 계속 돌지만
작업 진행 보고를 중단합니다. 이 변수는 매 App 시작 시 0으로 초기화됩니다.

1. 정상: 약 2초 후 APP_OK, 이후 30초 후 `app_runtime_result == BOOT_SUCCESS` 확인.
2. 부팅 실패: Confirm 전 `app_fault_injection=1` 설정. IWDG 리셋을 확인하고 매 시도마다
   같은 주입을 반복합니다. 3번째 실패 뒤 다음 부팅은 APP_FAIL/Recovery 대기여야 합니다.
3. 런타임 실패: Confirm 후 30초가 되기 전에 주입합니다. 각 재시도에서 다시 Confirm 후
   주입하여 세 번째 watchdog 리셋 시 Recovery 진입과 `runtime_failures=3`을 확인합니다.
4. 런타임 복구: 실패 횟수 1 또는 2인 상태에서 정상 동작 30초를 채워 0으로 초기화되는지 확인합니다.
5. 작업 지연: `app_fault_injection=2` 설정 후 약 1초 뒤 APP_HEALTH_FAULT가 latch되고
   watchdog 갱신이 중단되는지 확인합니다. 다시 0으로 바꿔도 갱신은 재개되지 않아야 합니다.

Debug 설정에서 IWDG freeze가 켜져 있으면 CPU halt 중에는 timeout이 멈출 수 있습니다.
리셋 시험에서는 CPU를 계속 실행하고, Bootloader의 RCC flag 읽기 이전에 멈추어
`boot_reset_flags`/`boot_reset_causes`와 최신 Metadata를 관찰하세요.

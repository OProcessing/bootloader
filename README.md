# STM32F446RE Bootloader 예제

STM32CubeIDE의 `bootFW`와 `AppFW` 프로젝트로 구성합니다. 2단계에서는
공유 Metadata ABI, Flash 저널, 부팅 시도 제한, App 확인 처리를 추가했습니다.

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
4. `APP_START`와 증가한 `boot_attempts`를 Flash에 저장한 뒤 App으로 점프합니다.
5. App 초기화 성공 후 `Boot_Confirm()`이 `APP_OK`, attempts=0을 저장합니다.
6. 확인 전에 리셋되면 다음 실행에서 재시도합니다. 최대 3번 App에 진입하며,
   세 번째 시도도 확인 없이 리셋되면 다음 부팅에 `APP_FAIL`을 기록하고 Bootloader에 머뭅니다.

전원 재인가, BOR, watchdog, software reset 등 모든 미확인 리셋은 시도를 소비합니다.
현재 예제의 App은 초기화 이후 처리할 주변장치가 없어 바로 확인합니다. 실제 제품에서는
핵심 주변장치 초기화 및 자체 진단을 마친 위치로 `Boot_Confirm()`을 옮기세요.
정상 확인 후의 다음 부팅은 attempts=1부터 다시 시작합니다.

이 단계에서는 watchdog을 켜지 않습니다. App이 멈춘 상태를 자동으로 리셋하려면
별도로 watchdog을 구성해야 합니다. 확인 없이 멈췄더라도 리셋이 발생하기 전에는
Bootloader가 실행되지 않으므로 재시도 횟수도 갱신되지 않습니다.

## Metadata 저장 규칙

`Shared/boot_state.h`의 `BootMetadata`는 64바이트 고정 구조체입니다.

- `boot_state`, `fw_version`, `fw_crc32`, `fw_size`
- `boot_attempts`, `total_boots`
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
| `Boot_MarkFailed()` | App 자체 진단 실패 시 APP_FAIL 영구 기록 |
| `Boot_InstallImage(version, size, crc)` | 향후 업데이트 경로에서 새 이미지 검증 후 등록 및 횟수 초기화 |

설치 API는 명시적 복구/업데이트 시에만 사용하며 매 부팅마다 호출하지 않습니다.
이미 APP_FAIL인 이미지는 자동 재시도하지 않습니다. 복구에는 검증된 이미지 재등록이 필요합니다.
새 이미지 기록 자체의 전원 차단 안전성 및 전송 프로토콜은 이 단계의 범위가 아닙니다.

Flash 쓰기 API는 인터럽트가 활성화되고 HAL SysTick이 실행 중인 thread mode에서만
호출합니다. ISR이나 동시 호출은 지원하지 않습니다. 두 프로젝트는 동시에 실행되지 않으며,
Bootloader는 Metadata 쓰기를 끝낸 후 IRQ/SysTick/NVIC를 정리하고 App으로 분기합니다.

디버거에서 Bootloader의 `boot_result`, `boot_metadata`, `boot_reset_flags`,
`boot_reset_causes`, App의 `app_confirm_result`를 관찰하세요. 정상 실행 결과는
`app_confirm_result == BOOT_SUCCESS`이고, Flash 최신 상태는 `APP_OK`, attempts=0입니다.
Bootloader의 `boot_metadata`는 점프 전 스냅샷이므로 App 확인 후 상태는
`Boot_MetadataRead()`나 Flash의 최신 레코드로 확인해야 합니다.

## 검증

호스트 테스트는 NOR Flash의 1→0 기록 제약을 모사합니다. MinGW GCC 예시:

```powershell
New-Item -ItemType Directory -Path build/tests -Force
gcc -std=c11 -O2 -Wall -Wextra -Werror Shared/boot_state.c tests/test_boot_state.c -o build/tests/test_boot_state.exe
./build/tests/test_boot_state.exe build/AppFW/AppFW.bin build/provision/metadata_a.bin
```

상태 전이, 3회 재시도, 확인 후 초기화, 이미지/Metadata CRC 오류, reset cause 비트,
sequence wrap, append/섹터 교체/confirm 중 80가지 word 기록 중단과 섹터 삭제 중단을 검사합니다.
실제 ARM App 바이너리와 Python 생성 Metadata를 C 코드로 읽어 boot/confirm하는 호환성도 확인합니다.

실제 보드에서는 정상 부팅, `Boot_Confirm()` 임시 생략 후 3회 시도와 네 번째 부팅 정지,
초기화 후 각 reset 원인, Metadata 쓰기/섹터 교체 중 전원 차단을 추가 확인해야 합니다.
자동화된 Flash 모사는 실제 저전압 시 Flash 동작을 대체하지 않습니다.

# 명령행 옵션 정리

이 장에서는 `Sat::ParseArgs()`가 인식하는 모든 옵션을 기능별로 정리합니다. 옵션 철자는 현재 소스 코드와 프로그램 도움말을 기준으로 표기합니다.

<sub><em>명령행 옵션: `Sat::ParseArgs()`가 명령행에서 인식하여 내부 설정값에 반영하는 문자열입니다.</em></sub>

## 코드에서 확인하는 옵션 처리 방식

> **파일:** `src/sat.cc` · **구간:** `ARG_KVALUE`, `ARG_IVALUE`, `Sat::ParseArgs()` · **기준:** `73b9df2`

```cpp
#define ARG_KVALUE(argument, variable, value) \
  if (!strcmp(argv[i], argument)) {           \
    variable = value;                         \
    continue;                                 \
  }

#define ARG_IVALUE(argument, variable)        \
  if (!strcmp(argv[i], argument)) {           \
    i++;                                      \
    if (i < argc)                             \
      variable = strtoull(argv[i], NULL, 0);  \
    continue;                                 \
  }

ARG_IVALUE("-M", size_mb_);
ARG_IVALUE("-s", runtime_seconds_);
ARG_IVALUE("-m", memory_threads_);
ARG_IVALUE("-i", invert_threads_);
ARG_IVALUE("-c", check_threads_);
```

**코드 설명:** 기존 정수 옵션 다수는 문자열 비교 매크로와 `strtoull(..., base=0)`를 사용합니다. 추가 진단 옵션과 `--printsec`, `--pause_delay`는 전체 문자열과 범위를 검사하는 10진수 parser를 사용합니다. 이 장의 표는 실제 옵션 처리 코드를 기준으로 작성했습니다.

<sub><em>Base 0 정수 변환: `0x` 접두사는 16진수, `0` 접두사는 8진수, 나머지는 10진수로 해석합니다.</em></sub>

## 이 fork에서 추가한 Android 시험 옵션

아래 옵션은 upstream `73b9df2`에 없으며 이 저장소에서 추가했습니다.

| 옵션 | 기본값 | 동작 |
|---|---:|---|
| `-P <ID\|이름[,ID\|이름...]>` | 무작위 선택 | 하나 또는 여러 pattern을 지정하고 block 선택마다 입력 순서로 순환. 여러 항목은 한 실행의 작업 단위에 혼합 |
| `--pattern-byte-offset <byte>` | 0 | Pattern 시작 위치를 4 byte 단위로 이동 |
| `--fill-preset <none\|zero\|one>` | `none` | 최종 pattern을 기록하기 전에 전체 영역을 지정값으로 기록 |
| `--prefault-pages` | 사용 안 함 | 각 SAT 작업 단위를 운영체제 page 크기 간격으로 순차 기록하고 미할당 주소의 물리 할당을 유도 |
| `--fill-threads <N>` | 8 | 초기 Fill Worker 수 지정. 종료 옵션 미지정 시 보조 종료 검사 Worker 수도 함께 지정. 허용 범위 1~256 |
| `--fill-direction <up\|down>` | `up` | SAT 작업 단위 내부의 Fill store 진행 방향 지정 |
| `--fill-verify-every <N>` | 사용 안 함 | SAT 작업 단위 번호를 기준으로 N 간격의 작업 단위를 즉시 검사 |
| `--fill-yield-bytes <bytes>` | 사용 안 함 | 지정한 byte 기록마다 `sched_yield()` 실행 |
| `--verify-after-fill` | 사용 안 함 | 초기 Fill 직후 전체 SAT 작업 단위를 한 번씩 검사 |
| `--post-fill-delay <seconds>` | 0 | Fill 종료 후 post-fill 검사와 runtime 시작 전까지 대기 |
| `--runtime-start-delay <seconds>` | 0 | Queue 구성 후 Runtime Worker 시작 전까지 대기 |
| `--copy-verify-destination` | 사용 안 함 | Copy destination을 기록 직후 검사 |
| `--invert-range <legacy\|full>` | `legacy` | 기존 공개 코드의 처리 범위 또는 SAT 작업 단위 전체를 Invert pass 범위로 선택 |
| `--diag-phase-summary` | 사용 안 함 | 해석된 실행 구성 2행과 단계별 block 수, 논리 read·write byte, 일반 memory word mismatch 수, 첫 오류 시각·DDR 요청 epoch 출력 |
| `--diag-vm-stats` | 사용 안 함 | 주요 단계 경계의 page fault, RSS, allocator와 mapping 정보 출력 |
| `--diag-block-history` | 사용 안 함 | Mismatch가 발생한 SAT 작업 단위의 마지막 추적 write 정보 출력 |
| `--error-log-limit <N>` | 제한 없음 | 오류 집계·reread·복구를 유지하고 상세 memory mismatch 로그를 N개로 제한 |
| `--final-check-threads <N>` | 명시하지 않음 | Runtime Check와 종료 검사를 분리하고 종료 검사 Worker 수 지정. 허용 범위 1~256 |
| `--skip-final-check` | 사용 안 함 | Runtime Check 종료 drain과 별도 종료 검사 생략 |
| `--ddr-freq <value\|list\|all>` | 제어 안 함 | 첫 값을 초기 Fill 전과 Runtime 직전에 요청. 여러 값의 순환은 Runtime에서 시작 |
| `--ddr-step <seconds>` | 3 | 여러 주파수를 사용할 때 변경 간격 지정 |
| `--ddr-node <path>` | Build 기본 경로 | 대상 시스템의 주파수 제어용 kernel interface 경로 지정 |
| `--dram-map lpddr-v1` | `none` | 오류의 시스템 물리 주소에 선택형 주소 변환 프로필 적용 |

`DIAG_CONFIG`, `DIAG_CONFIG_OPTIONS`, `DIAG_SUMMARY`, `DIAG_VM`과 `DIAG_ERROR_LOG` 요약은 `-v 5` 이상에서 출력됩니다. 기본 verbosity는 8입니다.

ID `27`과 이름 `OneZero256`은 같은 Pattern을 선택합니다. 소문자 `-p`는 SAT 작업 단위 크기 옵션입니다. Pattern 선택에는 대문자 `-P`를 사용합니다.

Pattern별 독립 비교는 프로세스마다 `-P` 항목 하나를 지정합니다. 쉼표 목록은 초기 Fill에서 여러 Pattern을 서로 다른 SAT 작업 단위에 배정합니다.

```bash
stressapptest -M 1024 -m 4 -i 4 -s 600 \
  -P OneZero256,FiveA256 \
  --dram-map lpddr-v1
```

주소 변환 결과는 대상 시스템의 memory-controller 설정과 memory topology를 기준으로 확인합니다. 오류 로그에서 시스템 물리 주소를 해석할 때 선택한 프로필을 적용합니다.

## 메모리 크기와 실행 시간

| 옵션 | 기본값 | 실제 동작과 모바일 환경에서의 의미 |
|---|---:|---|
| `-M <MiB>` | 자동 | 테스트 메모리 크기. 자동값이 전체 RAM에서 차지하는 비율이 크므로 Android에서는 직접 지정하는 것이 안전함 |
| `--reserve_memory <MiB>` | 0 | 크기를 자동 선택할 때 운영체제에 남길 최소 메모리. 실제 옵션에는 밑줄 사용 |
| `-H <MiB>` | 0 | 필요한 최소 huge page 메모리. 공통 코드는 huge page 하나를 2 MiB로 가정 |
| `-s <seconds>` | 20 | Runtime Worker 실행 시간. 초기 데이터 쓰기와 종료 시점 Valid 검사 시간은 제외 |
| `-p <bytes>` | 1,048,576 | SAT block 크기. 1,024 B 이상이며 2의 거듭제곱이어야 함 |
| `-m <N>` | online CPU 수 | `CopyThread` 수. 0은 Copy Worker 비활성 |
| `-i <N>` | 0 | `InvertThread` 수. 선택 범위에서 네 번의 read-modify-write와 cache 관리 명령 수행 |
| `-c <N>` | 0 | Runtime 중 Valid SAT 작업 단위를 검사하는 `CheckThread` 수 |
| `--fill-threads <N>` | 8 | 초기 전체 영역을 기록하는 Fill Worker 수. 종료 옵션 미지정 시 보조 종료 검사 Worker 수도 같은 값 사용. 1~256 사용 |
| `--fill-direction <up\|down>` | `up` | SAT 작업 단위 내부의 store 주소 진행 방향 |
| `--fill-verify-every <N>` | 사용 안 함 | SAT 작업 단위 번호 기준 N 간격의 작업 단위를 queue 반환 전에 검사. `1`은 전체 즉시 검사 |
| `--fill-yield-bytes <bytes>` | 사용 안 함 | 지정한 byte 기록마다 실행권 양보. 64 byte 배수이며 SAT 작업 단위 이하여야 함 |
| `--verify-after-fill` | 사용 안 함 | Valid/Empty 분류와 runtime Worker 시작 전에 전체 초기 Fill 결과 검사 |
| `--post-fill-delay <seconds>` | 0 | Fill 완료부터 첫 post-fill 검사 또는 runtime 시작까지의 대기 시간 |
| `--runtime-start-delay <seconds>` | 0 | Valid·Empty queue 구성부터 Runtime Worker 시작까지의 대기 시간 |
| `--copy-verify-destination` | 사용 안 함 | Copy 직후 destination SAT 작업 단위의 checksum 검사 |
| `--invert-range <legacy\|full>` | `legacy` | `legacy`는 기존 공개 코드의 pointer 단위 계산을 유지. `full`은 각 pass에서 `-p` 전체 처리 |
| `--diag-phase-summary` | 사용 안 함 | `DIAG_CONFIG`, `DIAG_CONFIG_OPTIONS`와 Worker local counter의 단계별 합계를 출력 |
| `--diag-vm-stats` | 사용 안 함 | 단계 경계에서 `getrusage()`와 확인 가능한 `/proc` VM 정보 출력 |
| `--diag-block-history` | 사용 안 함 | 작업 단위별 metadata에 마지막 추적 write 주체와 DDR 요청 epoch 저장. Fill·Copy는 전체 범위, Invert는 선택 범위 완료를 기록. CPU는 작업 완료 직후 표본 |
| `--final-check-threads <N>` | 명시하지 않음 | 지정 시 Runtime Check의 종료 drain을 중지하고 N개의 별도 종료 Check Worker 사용 |
| `--skip-final-check` | 사용 안 함 | Runtime Check 종료 drain과 최종 Check Worker 생성을 생략. `--final-check-threads`보다 우선 |
| `--fill-preset <none\|zero\|one>` | `none` | 최종 pattern Fill 전 데이터 상태 지정 |
| `--prefault-pages` | 사용 안 함 | 메인 스레드가 각 SAT 작업 단위의 offset 0부터 운영체제 page 크기 간격으로 기록한 뒤 Fill Worker 시작 |
| `--pattern-byte-offset <byte>` | 0 | Pattern 시작 위치를 이동. 0 이상의 4 byte 배수 사용 |
| `-C <N>` | 0 | `CpuStressThread` 수. 작은 데이터로 부동소수점 연산 부하 생성 |
| `-W` | 사용 안 함 | Vector 명령과 checksum을 사용하는 복사. ARM64에서는 일반 cacheable NEON `ld1/st1` 사용 |
| `-F` | 사용 안 함 | `-W` 미사용 시 Copy는 `memcpy()` 사용. Copy source, Invert 전·후, File·Network source/destination의 strict checksum 생략. `-W -F`에서는 Copy만 Warm checksum 경로 사용 |
| `-A` | 사용 안 함 | 호환성 및 debug 환경 검사를 완화. 공개 release build에서는 효과가 제한적 |

`CopyThread::Work()`는 `-W`의 `warm()` 조건을 먼저 확인합니다. `-W -F` 조합에서는 `CrcWarmCopyPage()`가 실행됩니다.

추가 옵션의 접근량 변화와 단계별 사용 순서는 [단계별 오류 검출과 옵션 영향 분석](18-stage-debugging-and-option-risk.md)에서 확인합니다.

### Invert 처리 범위

`--invert-range legacy`는 옵션을 지정하지 않은 실행의 기본값입니다. 기존 공개 코드가 사용한 pointer 단위 계산을 그대로 적용합니다.

| `-p` | `legacy`의 pass당 처리 범위 | `full`의 pass당 처리 범위 |
|---:|---:|---:|
| 1,024 B | 0 B | 1,024 B |
| 2,048 B | 0 B | 2,048 B |
| 4,096 B | 2,048 B | 4,096 B |
| 1 MiB | 512 KiB | 1 MiB |

새로운 전체 범위 시험은 `--invert-range full`을 명시합니다. 기존 바이너리와 비교하는 기준 실행은 `legacy`를 사용합니다. 두 모드는 Invert read·write byte와 실행 시간이 다릅니다.

### DRAM 주파수 제어 옵션

| 옵션 | 동작 |
|---|---|
| `--ddr-freq <value>` | 지정한 한 값을 적용하고 fixed mode로 실행 |
| `--ddr-freq <value1,value2,...>` | 입력한 값을 `--ddr-step` 간격으로 순환하는 sweep mode 실행 |
| `--ddr-freq all` | Build에 등록된 전체 주파수 목록을 순서대로 순환 |
| `--ddr-step <seconds>` | Sweep mode의 주파수 유지 시간 지정. 기본값 3초 |
| `--ddr-node <path>` | 주파수 제어용 kernel interface 경로 지정 |

첫 값은 초기 Fill 전과 Runtime 직전에 요청합니다. 여러 값의 순환은 Runtime에서 시작합니다. `--ddr-freq`를 생략하면 DRAM 주파수 제어를 수행하지 않습니다. 한 값을 지정하면 `cur_mode:fixed`, 여러 값을 지정하면 `cur_mode:sweep`이 mismatch 상세 로그에 기록됩니다. `cur_freq`는 reread 직전에 프로그램이 확인한 마지막 전달값입니다. 실제 적용값은 kernel trace, debug interface 또는 hardware counter로 확인합니다.

현재 구현은 `--ddr-node` 경로에 `{class:ddr, res:fixed, val:<값>}` 형식의 한 줄을 기록합니다. 숫자만 받는 sysfs 파일에는 사용할 수 없습니다. `<값>`의 단위와 허용 범위는 대상 interface 규격을 따릅니다.

<sub><em>시스템 계측값: kernel trace, debug interface 또는 hardware counter에서 확인한 실제 동작 상태입니다.</em></sub>

### `--reserve_memory` 사용 형식

실행 parser와 도움말은 `--reserve_memory`를 사용합니다.

```bash
stressapptest --reserve_memory 1024
```

## Block 관리 방식과 CPU 배치

| 옵션 | 기본값 | 실제 동작과 모바일 환경에서의 의미 |
|---|---:|---|
| `--coarse_grain_lock` | 사용 안 함 | Empty·valid queue 전체를 각각 하나의 mutex로 보호. 기본 block별 mutex 방식과 비교하는 숨은 옵션 |
| `--no_affinity` | 사용 안 함 | OS scheduler가 CPU 배치를 결정하도록 설정 |
| `--local_numa` | 사용 안 함 | 각 Copy Worker를 한 region의 CPU에 배치하고 같은 region tag의 SAT 작업 단위를 선택 |
| `--remote_numa` | 사용 안 함 | 각 Copy Worker를 한 region의 CPU에 배치하고 다른 region tag의 SAT 작업 단위를 선택 |

공통 Android AArch64 build의 `OsLayer::FindRegion()`은 시스템 물리 주소 범위를 내부 region tag로 나눕니다. 이 tag는 stressapptest 내부 block 선택에 사용합니다. Linux NUMA topology와 LPDDR channel 좌표는 대상 시스템에 맞는 `OsLayer` 구현에서 제공합니다.

<sub><em>NUMA: CPU와 memory node의 topology에 따라 memory access latency와 bandwidth가 달라지는 구조입니다.</em></sub>
<sub><em>Region tag: local/remote block 선택에 사용하는 stressapptest 내부 bit mask입니다.</em></sub>

## Cache 일관성과 CPU 주파수 검사

| 옵션 | 기본값 | 실제 동작과 모바일 환경에서의 의미 |
|---|---:|---|
| `--cc_test` | 사용 안 함 | 설정한 CPU 수만큼 cache coherency thread 생성 |
| `--cc_inc_count <N>` | 1000 | 한 번의 coherency 검사에서 counter를 증가시키는 횟수 |
| `--cc_line_count <N>` | 2 | 공동으로 사용하는 cache line 크기 구조체 수 |
| `--cc_line_size <bytes>` | 0/자동 | Cache line 크기를 직접 지정할 값 |
| `--cpu_freq_test` | 사용 안 함 | x86 TSC·APERF·MPERF를 이용한 주파수 검사 활성화 |
| `--cpu_freq_threshold <MHz>` | 0 | 주파수 합격 기준의 최솟값. 검사를 켜면 양수 필요 |
| `--cpu_freq_round <MHz>` | 10 | 계산한 주파수를 반올림할 단위. 0이면 1 MHz 단위 |

공통 AArch64 build에서 `--cpu_freq_test`를 사용하면 지원되지 않는 옵션이라는 메시지를 출력하고 초기화를 종료합니다.

## 검증과 오류 처리

| 옵션 | 기본값 | 실제 동작과 모바일 환경에서의 의미 |
|---|---:|---|
| `--max_errors <N>` | 0 | 0이면 제한 없음. 전체 오류 수가 N을 초과하면 주 실행 반복 종료 |
| `--stop_on_errors` | 사용 안 함 | Mismatch 복구와 로그 기록 후 종료 요청. 초기 검사에서는 Fill과 queue 구성 후 Runtime 생략. Runtime Memory Worker는 현재 SAT 작업 단위를 반환하고, 다른 Runtime Worker도 현재 반복의 정리 지점에서 종료. 종료 검사 생략 |
| `--no_errors` | 사용 안 함 | `ErrorPollThread` 비활성. Pattern checksum 검사는 유지 |
| `--force_errors` | 사용 안 함 | 프로그램이 의도적으로 오류를 만들어 오류 처리 기능 검사 |
| `--force_errors_like_crazy` | 사용 안 함 | 데이터와 상태 정보를 반복해서 바꾸어 많은 오류 생성 |
| `--tag_mode` | 사용 안 함 | 각 64 B cache line의 첫 8 B에 가상 주소 tag 사용. `-f`, `-d`, `-n`과 함께 지정하면 초기화 실패 |

`--no_errors`는 `ErrorPollThread` 생성을 생략합니다. 복사 중 checksum은 `-F`로 제어하며 종료 시점의 Valid 검사는 유지됩니다.

<sub><em>Error polling: platform error register 또는 kernel interface를 주기적으로 조회하는 동작입니다.</em></sub>

## 로그와 실행 상태 제어

| 옵션 | 기본값 | 실제 동작과 모바일 환경에서의 의미 |
|---|---:|---|
| `-l <path>` | 없음 | 동기 쓰기 속성으로 로그 파일을 열고 기존 내용의 끝에 추가 |
| `-v <0..20>` | 8 | 출력할 로그의 상세 수준. 값이 클수록 더 많은 로그를 출력 |
| `--printsec <seconds>` | 10 | 남은 시간을 출력하는 간격. 1 이상의 값을 사용 |
| `--no_timestamps` | 사용 안 함 | Timestamp 없는 로그 형식 사용 |
| `--pause_delay <seconds>` | 600 | 부하 Worker의 일시 정지 주기. 1 이상의 값 사용 |
| `--pause_duration <seconds>` | 15 | 일시 정지를 유지하는 시간 |

일시 정지와 재시작은 `power_spike_status` 그룹에 등록된 Worker에 적용됩니다. `continuous_status` 그룹의 Worker는 계속 실행됩니다.

## 물리 주소와 channel 추정

| 옵션 | 기본값 | 실제 동작과 모바일 환경에서의 의미 |
|---|---:|---|
| `--do_page_map` | 사용 안 함 | `/proc/self/pagemap`을 이용하여 접근한 4 KiB 물리 페이지를 bitmap으로 출력. Android에서는 제한적이며 프로그램 중단 가능 |
| `--paddr_base <address>` | 0 | 공통 `OsLayer`는 기본값 0만 지원하며 다른 입력을 무시 |
| `--channel_hash <mask>` | `0x40` | mask bit parity/XOR로 2-channel 선택 추정 |
| `--channel_width <bits>` | 64 | channel 폭. power-of-two, 최소 16 |
| `--memory_channel <a,b,...>` | 없음 | 한 channel에 속한 package 이름. 1~2회 반복 지정 가능 |

`--memory_channel`은 channel별로 같은 package 수와 2의 거듭제곱 구성을 요구합니다. Package당 폭은 x8 이상입니다. 최신 모바일 LPDDR 주소 배치는 대상 시스템의 address map으로 확인합니다.

서버형 메모리 구성을 가정한 예시는 다음과 같습니다.

```bash
stressapptest \
  --memory_channel ch0a,ch0b \
  --memory_channel ch1a,ch1b \
  --channel_width 64 \
  --channel_hash 0x40
```

## 파일 I/O

| 옵션 | 기본값 | 실제 동작과 모바일 환경에서의 의미 |
|---|---:|---|
| `-f <filename>` | 없음 | `FileThread` 하나 추가. 반복 지정 가능 |
| `--filesize <bytes>` | 8 MiB | `FileThread`가 한 번에 처리할 파일 크기. `-p` 변경 시 내부 block 수를 다시 계산 |
| `--findfiles` | 사용 안 함 | `FindFileDevices()`로 파일 경로를 자동 검색. 공통 `OsLayer`에서는 빈 목록 반환 |

`-f`는 파일을 `O_SYNC`와 가능한 경우 `O_DIRECT`로 열고 쓰기, 읽기, sector 정보 검사를 수행합니다. Filesystem의 남은 공간과 저장 장치 수명을 고려해야 합니다.

## 네트워크 I/O

| 옵션 | 기본값 | 실제 동작과 모바일 환경에서의 의미 |
|---|---:|---|
| `-n <IPv4>` | 없음 | 지정한 IP의 TCP port 19996에 연결하는 `NetworkThread` 추가. 반복 가능 |
| `--listen` | 사용 안 함 | TCP port 19996에서 연결을 기다리고 연결마다 데이터를 되돌려 보내는 thread 생성 |

코드 주석에는 hostname이라고 적혀 있지만 실제 주소 변환 함수는 `inet_aton()`입니다. 따라서 점으로 구분한 IPv4 주소를 입력해야 합니다. 송신 측은 프로그램 시작 후 15초를 기다린 뒤 연결합니다.

## Block device 직접 I/O

| 옵션 | 기본값 | 실제 동작과 모바일 환경에서의 의미 |
|---|---:|---|
| `-d <device-or-file>` | 없음 | `DiskThread` 하나 추가. 반복 지정 가능 |
| `--destructive` | 사용 안 함 | `DiskThread` 쓰기 활성화. 실제 시험 대상의 데이터를 덮어씀 |
| `--read-block-size <bytes>` | 512 | 읽기 단위. 512 B의 배수 |
| `--write-block-size <bytes>` | 읽기 크기 | 쓰기 단위. 512 B와 읽기 크기의 배수 |
| `--segment-size <bytes>` | 전체 장치 | 주소를 분산할 segment 크기. 512 B의 배수 |
| `--cache-size <bytes>` | 16 MiB | 장치 cache 추정 크기. 동시에 진행할 I/O 수를 계산할 때 사용 |
| `--blocks-per-segment <N>` | 32 | 한 segment에서 처리할 block 수 |
| `--read-threshold <us>` | 100,000 | 읽기 시간이 이 값을 넘으면 느린 읽기 경고 출력 |
| `--write-threshold <us>` | 100,000 | 쓰기 시간이 이 값을 넘으면 느린 쓰기 경고 출력 |
| `--random-threads <N>` | 0 | 각 `DiskThread`에 추가할 임의 위치 읽기 Worker 수 |

### Block device를 덮어쓰는 조건

```bash
stressapptest -d /dev/block/... --destructive
```

이 명령은 해당 block device의 일부를 실제로 덮어씁니다. 데이터 폐기가 승인된 전용 시험 partition에서 실행하며 image backup과 복구 절차를 먼저 준비합니다.

현재 `--random-threads` 실행 경로에는 initialized 상태를 설정하는 함수가 빠진 것으로 보이는 문제가 있습니다. 대상 build에서 실제 상태 변경을 확인한 뒤 사용해야 합니다.

## 오류 감시 전용 방식과 도움말

| 옵션 | 기본값 | 실제 동작과 모바일 환경에서의 의미 |
|---|---:|---|
| `--monitor_mode` | 사용 안 함 | 테스트 메모리 할당과 부하 없이 `ErrorPollThread`만 실행 |
| `-h`, `--help` | - | 프로그램 version과 기본 도움말을 출력하고 종료 코드 0으로 종료 |

공통 ARM `ErrorPoll()`은 항상 0을 반환합니다. `--monitor_mode`에서 corrected ECC 오류를 수집하려면 해당 SoC에 맞는 `OsLayer::ErrorPoll()` 구현이 필요합니다.

## 도움말에는 없지만 실제 코드가 인식하는 옵션

현재 기본 도움말에서 누락된 주요 옵션은 다음 항목입니다.

- `--coarse_grain_lock`
- `--tag_mode`
- `--do_page_map`

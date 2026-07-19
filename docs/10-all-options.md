# 명령행 옵션 정리

이 장에서는 `Sat::ParseArgs()`가 실제로 인식하는 모든 옵션을 기능별로 정리합니다. 프로그램 도움말과 실제 옵션 처리 코드의 철자가 다른 경우도 표시합니다.

<sub><em>Parser option: `Sat::ParseArgs()`가 command line에서 인식하여 내부 설정값에 반영하는 문자열입니다.</em></sub>

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

**코드 설명:** 옵션은 GNU `getopt` 표가 아니라 문자열을 비교하는 macro로 처리합니다. 정수는 base 0의 `strtoull()`로 읽으므로 10진수와 `0x`로 시작하는 16진수를 사용할 수 있습니다. 이 장의 표는 프로그램 도움말이 아니라 실제 옵션 처리 코드를 기준으로 작성했습니다.

<sub><em>Base 0 integer parsing: 숫자 prefix에 따라 `0x`는 16진수, 앞의 `0`은 8진수, 그 외에는 10진수로 해석하는 C library 변환 방식입니다.</em></sub>

## 메모리 크기와 실행 시간

| 옵션 | 기본값 | 실제 동작과 모바일 환경에서의 의미 |
|---|---:|---|
| `-M <MiB>` | 자동 | 테스트 메모리 크기. 자동값이 전체 RAM에서 차지하는 비율이 크므로 Android에서는 직접 지정하는 것이 안전함 |
| `--reserve_memory <MiB>` | 0 | 크기를 자동 선택할 때 운영체제에 남길 최소 메모리. 실제 옵션에는 밑줄 사용 |
| `-H <MiB>` | 0 | 필요한 최소 huge page 메모리. 공통 코드는 huge page 하나를 2 MiB로 가정 |
| `-s <seconds>` | 20 | 본 시험의 Worker 실행 시간. 초기 데이터 쓰기와 마지막 전체 검사 시간은 제외 |
| `-p <bytes>` | 1,048,576 | SAT block 크기. 1,024 B 이상이며 2의 거듭제곱이어야 함 |
| `-m <N>` | online CPU 수 | `CopyThread` 수. 0이면 복사 Worker를 생성하지 않음 |
| `-i <N>` | 0 | `InvertThread` 수. 네 번의 read-modify-write와 cache 관리 명령 수행 |
| `-c <N>` | 0 | `CheckThread` 수. 도움말에는 없지만 실제 코드에서 지원 |
| `-C <N>` | 0 | `CpuStressThread` 수. 작은 데이터로 부동소수점 연산 부하 생성 |
| `-W` | 사용 안 함 | Vector 명령과 checksum을 사용하는 복사. ARM64에서는 일반 cacheable NEON `ld1/st1` 사용 |
| `-F` | 사용 안 함 | 복사 중 checksum을 생략하고 `CopyThread`에서 C library의 `memcpy()` 사용 |
| `-A` | 사용 안 함 | 호환되지 않는 환경과 debug 환경 검사를 완화. 공개 release build에서는 효과가 제한적 |

`CopyThread::Work()`는 `-W`에 해당하는 `warm()` 조건을 먼저 확인합니다. 따라서 `-W -F`를 함께 지정하면 `CrcWarmCopyPage()`가 실행되고 C library의 `memcpy()`는 실행되지 않습니다.

### `--reserve_memory` 철자 주의

`PrintHelp()`는 `--reserve-memory`를 출력하지만 실제 코드는 `--reserve_memory`를 인식합니다. 실행 명령에는 다음과 같이 밑줄을 사용해야 합니다.

```bash
stressapptest --reserve_memory 1024
```

## Block 관리 방식과 CPU 배치

| 옵션 | 기본값 | 실제 동작과 모바일 환경에서의 의미 |
|---|---:|---|
| `--coarse_grain_lock` | 사용 안 함 | Empty·valid queue 전체를 각각 하나의 mutex로 보호. 기본 block별 mutex 방식과 비교하는 숨은 옵션 |
| `--no_affinity` | 사용 안 함 | `sched_setaffinity()`를 적용하지 않음 |
| `--local_numa` | 사용 안 함 | Worker와 같은 공통 메모리 구역 tag의 block 선택 |
| `--remote_numa` | 사용 안 함 | Worker와 다른 메모리 구역 tag의 block 선택 |

공통 모바일 build의 메모리 구역 tag에는 실제 NUMA 또는 LPDDR channel 구조가 포함되어 있지 않습니다. 실제 위치 관계를 분석하려면 SoC 제조사의 구조를 반영한 `OsLayer` 구현이 필요합니다.

<sub><em>NUMA: CPU와 memory node의 topology에 따라 memory access latency와 bandwidth가 달라지는 구조입니다.</em></sub>
<sub><em>Region tag: local/remote block 선택에 사용하는 stressapptest 내부 bit mask입니다.</em></sub>

## Cache 일관성과 CPU 주파수 검사

| 옵션 | 기본값 | 실제 동작과 모바일 환경에서의 의미 |
|---|---:|---|
| `--cc_test` | 사용 안 함 | 설정한 CPU 수만큼 cache coherency thread 생성 |
| `--cc_inc_count <N>` | 1000 | 한 번의 coherency 검사에서 counter를 증가시키는 횟수 |
| `--cc_line_count <N>` | 2 | 공동으로 사용하는 cache line 크기 구조체 수 |
| `--cc_line_size <bytes>` | 0/자동 | 자동으로 확인한 cache line 크기 대신 사용할 값 |
| `--cpu_freq_test` | 사용 안 함 | x86 TSC·APERF·MPERF를 이용한 주파수 검사 활성화 |
| `--cpu_freq_threshold <MHz>` | 0 | 주파수 합격 기준의 최솟값. 검사를 켜면 양수 필요 |
| `--cpu_freq_round <MHz>` | 10 | 계산한 주파수를 반올림할 단위. 0이면 1 MHz 단위 |

`--cpu_freq_test`는 AArch64에서 지원하지 않으므로 초기화가 실패합니다.

## 검증과 오류 처리

| 옵션 | 기본값 | 실제 동작과 모바일 환경에서의 의미 |
|---|---:|---|
| `--max_errors <N>` | 0 | 0이면 제한 없음. 전체 오류 수가 N을 초과하면 주 실행 반복 종료 |
| `--stop_on_errors` | 사용 안 함 | 지원되는 오류 처리 경로에서 첫 오류가 발생하면 즉시 중단 |
| `--no_errors` | 사용 안 함 | `ErrorPollThread`를 생성하지 않음. Pattern checksum 검사는 유지 |
| `--force_errors` | 사용 안 함 | 프로그램이 의도적으로 오류를 만들어 오류 처리 기능 검사 |
| `--force_errors_like_crazy` | 사용 안 함 | 데이터와 상태 정보를 반복해서 바꾸어 많은 오류 생성 |
| `--tag_mode` | 사용 안 함 | 각 64 B cache line의 첫 8 B에 virtual address tag 사용. 파일·네트워크 옵션과 함께 사용 불가 |

`--no_errors`는 `ErrorPollThread`만 생성하지 않게 합니다. 복사 중 checksum 검사는 `-F`로 제어하며 마지막 전체 검사는 유지됩니다.

<sub><em>Error polling: platform error register 또는 kernel interface를 주기적으로 조회하는 동작입니다.</em></sub>

## 로그와 실행 상태 제어

| 옵션 | 기본값 | 실제 동작과 모바일 환경에서의 의미 |
|---|---:|---|
| `-l <path>` | 없음 | 동기 쓰기 속성으로 로그 파일을 열고 기존 내용의 끝에 추가 |
| `-v <0..20>` | 8 | verbosity threshold |
| `--printsec <seconds>` | 10 | 남은 시간을 출력하는 간격. 내부 계산 문제를 피하려면 0이 아닌 값 사용 |
| `--no_timestamps` | 사용 안 함 | 로그 앞에 시각을 표시하지 않음 |
| `--pause_delay <seconds>` | 600 | 부하를 만드는 Worker를 일시 정지하는 주기. 0은 사용하지 않음 |
| `--pause_duration <seconds>` | 15 | 일시 정지를 유지하는 시간 |

일시 정지와 재시작은 `power_spike_status` 그룹에 등록된 Worker에 적용됩니다. `continuous_status` 그룹의 Worker는 계속 실행됩니다.

## Physical address와 channel 추정

| 옵션 | 기본값 | 실제 동작과 모바일 환경에서의 의미 |
|---|---:|---|
| `--do_page_map` | 사용 안 함 | `/proc/self/pagemap`을 이용하여 접근한 4 KiB physical page를 bitmap으로 출력. Android에서는 제한적이며 프로그램 중단 가능 |
| `--paddr_base <address>` | 0 | 공통 `OsLayer`에서는 0이 아닌 값을 무시 |
| `--channel_hash <mask>` | `0x40` | mask bit parity/XOR로 2-channel 선택 추정 |
| `--channel_width <bits>` | 64 | channel 폭. power-of-two, 최소 16 |
| `--memory_channel <a,b,...>` | 없음 | 한 channel에 속한 package 이름. 1~2회 반복 지정 가능 |

`--memory_channel`을 사용할 때에는 channel별 package 수가 같고 그 수가 2의 거듭제곱이어야 합니다. Package당 폭은 x8 이상이어야 합니다. 공통 계산 방식은 최신 모바일 LPDDR 주소 배치를 자동으로 확인하지 못합니다.

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
| `--filesize <bytes>` | 8 × block 크기 | `FileThread`가 한 번에 처리할 파일 크기 |
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

이 명령은 해당 block device의 일부를 실제로 덮어씁니다. 데이터 폐기가 승인된 전용 시험 partition에서만 사용해야 하며, 실행 전에 image backup과 복구 절차를 준비해야 합니다. Userdata, filesystem, boot, metadata partition에는 사용하면 안 됩니다.

현재 `--random-threads` 실행 경로에는 initialized 상태를 설정하는 함수가 빠진 것으로 보이는 문제가 있습니다. 대상 build에서 실제 상태 변경을 확인한 뒤 사용해야 합니다.

## 오류 감시 전용 방식과 도움말

| 옵션 | 기본값 | 실제 동작과 모바일 환경에서의 의미 |
|---|---:|---|
| `--monitor_mode` | 사용 안 함 | 테스트 메모리 할당과 부하 없이 `ErrorPollThread`만 실행 |
| `-h`, `--help` | - | 프로그램 version과 기본 도움말을 출력하고 종료 코드 0으로 종료 |

공통 ARM `ErrorPoll()`은 항상 0을 반환합니다. `--monitor_mode`에서 corrected ECC 오류를 수집하려면 해당 SoC에 맞는 `OsLayer::ErrorPoll()` 구현이 필요합니다.

## 도움말에는 없지만 실제 코드가 인식하는 옵션

현재 기본 도움말에서 누락된 주요 옵션은 다음과 같습니다.

- `-c`
- `--coarse_grain_lock`
- `--tag_mode`
- `--do_page_map`

`--reserve_memory`는 도움말과 실제 코드의 철자가 다릅니다. 자동 실행 script에는 분석한 commit의 `Sat::ParseArgs()`가 인식하는 문자열을 사용해야 합니다.

# 모든 명령행 옵션

이 표는 built-in help가 아니라 `Sat::ParseArgs()`의 실제 문자열 비교를 기준으로 작성했다. 현재 parser에는 help에서 빠진 option과 철자가 다른 option이 있다.

## Memory와 실행 시간

| Option | 기본값 | 실제 동작 및 모바일 해석 |
|---|---:|---|
| `-M <MiB>` | 자동 | test memory 크기. 자동값은 total RAM 기준 비율이 커서 Android에서는 명시 권장 |
| `--reserve_memory <MiB>` | 0 | 자동 memory 선택 시 system에 남길 최소 크기. 실제 parser는 underscore 사용 |
| `-H <MiB>` | 0 | 최소 huge page memory 요구. generic code는 huge page 하나를 2 MiB로 가정 |
| `-s <seconds>` | 20 | timed worker 실행 시간. 초기 fill/final check 시간은 제외 |
| `-p <bytes>` | 1,048,576 | SAT block 크기. 1,024 B 이상 power-of-two여야 함 |
| `-m <N>` | online CPU 수 | CopyThread 수. 0이면 copy worker 없음 |
| `-i <N>` | 0 | InvertThread 수. RMW 네 pass와 cache maintenance 수행 |
| `-c <N>` | 0 | CheckThread 수. help에는 빠져 있지만 parser에서 지원 |
| `-C <N>` | 0 | CpuStressThread 수. small FP working set 계산 부하 |
| `-W` | off | warm/vector checksum copy. ARM64에서는 cached NEON `ld1/st1` |
| `-F` | off | per-transaction strict checksum을 끄고 CopyThread는 libc `memcpy` 사용 |
| `-A` | off | incompatible/debug environment check를 완화. open-source release build에서는 효과가 제한적 |

`CopyThread::Work()`는 `-W`를 먼저 검사하고 그 다음 strict 여부를 검사한다. 따라서 `-W -F`를 같이 지정하면 `-W`의 checksum copy가 실행되며, `-F`의 libc `memcpy()` 경로는 선택되지 않는다.

### `--reserve_memory` 철자 주의

`PrintHelp()`는 `--reserve-memory`라고 출력하지만 parser는 `--reserve_memory`만 비교한다. 현재 commit에서는 다음처럼 사용해야 한다.

```bash
stressapptest --reserve_memory 1024
```

## Queue, affinity, region

| Option | 기본값 | 실제 동작 및 모바일 해석 |
|---|---:|---|
| `--coarse_grain_lock` | off | per-entry fine-lock 대신 empty/valid global-lock queue 사용. 성능 비교용 숨은 option |
| `--no_affinity` | off | `sched_setaffinity`를 적용하지 않음 |
| `--local_numa` | off | worker와 같은 generic region tag의 block 선택 |
| `--remote_numa` | off | worker region이 아닌 block 선택 |

generic mobile build는 실제 NUMA/LPDDR channel topology를 구현하지 않으므로 local/remote 결과를 hardware locality로 해석하면 안 된다.

## Cache coherency와 CPU frequency

| Option | 기본값 | 실제 동작 및 모바일 해석 |
|---|---:|---|
| `--cc_test` | off | configured CPU 수만큼 cache coherency thread 생성 |
| `--cc_inc_count <N>` | 1000 | 각 coherency batch에서 counter 증가 횟수 |
| `--cc_line_count <N>` | 2 | 공유 cache-line-sized structure 수 |
| `--cc_line_size <bytes>` | 0/자동 | auto-detected cache line size override |
| `--cpu_freq_test` | off | x86 TSC/APERF/MPERF frequency test 활성화 |
| `--cpu_freq_threshold <MHz>` | 0 | frequency pass/fail 하한. test 활성화 시 양수 필요 |
| `--cpu_freq_round <MHz>` | 10 | 계산 frequency 반올림 단위. 0이면 1 MHz 단위 |

`--cpu_freq_test`는 AArch64에서 지원되지 않아 초기화가 실패한다.

## 검증과 오류 처리

| Option | 기본값 | 실제 동작 및 모바일 해석 |
|---|---:|---|
| `--max_errors <N>` | 0 | 0은 제한 없음. total errors가 N을 초과하면 main loop 종료 |
| `--stop_on_errors` | off | 일부 error path에서 첫 오류 즉시 중단/exit |
| `--no_errors` | off | ErrorPollThread 생성 안 함. pattern checksum 검사는 유지 |
| `--force_errors` | off | software corruption을 주입해 error path 검증 |
| `--force_errors_like_crazy` | off | 반복적인 강한 software error injection |
| `--tag_mode` | off | 각 64 B line 첫 8 B를 virtual-address tag로 사용. disk/network와 동시 사용 불가 |

`--no_errors`를 “memory 검증 끄기”로 이해하면 안 된다. 이동 중 checksum을 끄는 option은 `-F`다.

## Logging과 진행 제어

| Option | 기본값 | 실제 동작 및 모바일 해석 |
|---|---:|---|
| `-l <path>` | 없음 | sync flag로 log file open 후 기존 끝에 추가 |
| `-v <0..20>` | 8 | verbosity threshold |
| `--printsec <seconds>` | 10 | remaining-time log 간격. 0은 내부 주기 계산에 부적절하므로 양수 사용 |
| `--no_timestamps` | off | log prefix timestamp 비활성화 |
| `--pause_delay <seconds>` | 600 | power-spike worker pause 간격. 0은 사용하지 말 것 |
| `--pause_duration <seconds>` | 15 | pause 유지 시간 |

pause/resume은 모든 worker가 아니라 `power_spike_status` 그룹에 적용된다.

## Physical address 및 channel 진단

| Option | 기본값 | 실제 동작 및 모바일 해석 |
|---|---:|---|
| `--do_page_map` | off | `/proc/self/pagemap` 기반 4 KiB physical coverage bitmap 출력. Android에서 제한적/위험 |
| `--paddr_base <address>` | 0 | generic OsLayer에서는 non-zero 값 무시 |
| `--channel_hash <mask>` | `0x40` | mask bit parity/XOR로 2-channel 선택 추정 |
| `--channel_width <bits>` | 64 | channel 폭. power-of-two, 최소 16 |
| `--memory_channel <a,b,...>` | 없음 | 한 channel의 package 이름. 1~2회 반복 지정 가능 |

`--memory_channel` 사용 시 channel별 package 수가 같고 power-of-two여야 하며 package당 폭은 x8 이상이어야 한다. 이 generic model은 최신 mobile LPDDR map을 자동 검출하지 않는다.

예전 server형 예시:

```bash
stressapptest \
  --memory_channel ch0a,ch0b \
  --memory_channel ch1a,ch1b \
  --channel_width 64 \
  --channel_hash 0x40
```

## File I/O

| Option | 기본값 | 실제 동작 및 모바일 해석 |
|---|---:|---|
| `-f <filename>` | 없음 | FileThread 하나 추가. 반복 지정 가능 |
| `--filesize <bytes>` | 8 × block size | FileThread가 한 pass에서 사용할 파일 크기 |
| `--findfiles` | off | `FindFileDevices()`로 자동 path 탐색. generic OsLayer에서는 빈 목록 |

`-f`는 파일을 `O_SYNC` 및 가능한 경우 `O_DIRECT`로 열고 write/read/sector-tag/verify한다. filesystem 공간과 수명을 고려해야 한다.

## Network I/O

| Option | 기본값 | 실제 동작 및 모바일 해석 |
|---|---:|---|
| `-n <IPv4>` | 없음 | 지정 IP:19996으로 연결하는 NetworkThread 추가. 반복 가능 |
| `--listen` | off | TCP 19996 listener 및 connection별 reflector thread |

parser comment는 hostname이라고 하지만 실제 연결 변환은 `inet_aton()`이므로 dotted IPv4 주소를 사용한다. sender는 시작 후 15초 대기한다.

## Direct disk/device I/O

| Option | 기본값 | 실제 동작 및 모바일 해석 |
|---|---:|---|
| `-d <device-or-file>` | 없음 | DiskThread 하나 추가. 반복 지정 가능 |
| `--destructive` | off | DiskThread write 활성화. 실제 target 내용을 덮어씀 |
| `--read-block-size <bytes>` | 512 | read 단위, 512 B 배수 |
| `--write-block-size <bytes>` | read size | write 단위, 512 B 및 read size 배수 |
| `--segment-size <bytes>` | 전체 device | address 분산 segment 크기, 512 B 배수 |
| `--cache-size <bytes>` | 16 MiB | device cache 추정값. queue depth 계산에 사용 |
| `--blocks-per-segment <N>` | 32 | 한 segment에서 처리할 block 수 |
| `--read-threshold <us>` | 100,000 | 넘으면 slow-read warning |
| `--write-threshold <us>` | 100,000 | 넘으면 slow-write warning |
| `--random-threads <N>` | 0 | 각 DiskThread의 shared block random reader 수 |

### 매우 중요한 안전성

```bash
stressapptest -d /dev/block/... --destructive
```

는 해당 block device 일부를 실제로 덮어쓴다. userdata, filesystem, boot, metadata partition에서 사용하면 안 된다. disposable test partition과 복구 절차가 확실한 환경에서만 사용한다.

현재 `--random-threads` 경로는 initialized flag setter 호출 누락으로 보이는 구현상 문제가 있으므로 target에서 사전 검증해야 한다.

## Monitor와 help

| Option | 기본값 | 실제 동작 및 모바일 해석 |
|---|---:|---|
| `--monitor_mode` | off | memory allocation/stress 없이 ErrorPollThread만 실행 |
| `-h`, `--help` | - | version과 built-in help 출력 후 exit 0 |

generic ARM `ErrorPoll()`은 no-op이므로 `--monitor_mode`는 vendor-specific OsLayer 없이 corrected ECC monitor 역할을 하지 못한다.

## Help에 나타나지 않는 parser option

현재 built-in help에서 누락된 주요 option은 다음과 같다.

- `-c`
- `--coarse_grain_lock`
- `--tag_mode`
- `--do_page_map`

그리고 `--reserve_memory`는 help와 parser 철자가 다르다. 자동화 script는 `--help` output만 믿지 말고 target commit의 parser를 확인해야 한다.

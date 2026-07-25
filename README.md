# StressAppTest Android ARM64 확장판

이 저장소는 공개 [stressapptest](https://github.com/stressapptest/stressapptest)에 Android AArch64 실장기 테스트 기능을 추가한 개인 fork입니다. 이 README는 이 fork에서 추가한 기능, 기존 stressapptest 옵션, Android 실행 방법과 오류 로그 해석 방법을 함께 설명합니다.

[![Android ARM64 최신 버전 다운로드](https://img.shields.io/badge/Android_ARM64-최신_버전_다운로드-1976d2?style=for-the-badge&logo=android&logoColor=white)](https://github.com/stpcoder/stressapptest/releases/latest/download/stressapptest-android-arm64)

압축 파일과 SHA-256 checksum이 필요하면 [최신 Release 페이지](https://github.com/stpcoder/stressapptest/releases/latest)에서 다운로드할 수 있습니다.

## 추가된 기능

| 기능 | 옵션 | 설명 |
|---|---|---|
| 패턴 선택 | `-P <ID\|이름[,ID\|이름...]>` | 하나 또는 여러 pattern을 지정한 순서대로 block에 순환 배정 |
| DDR 주파수 고정 | `--ddr-freq <주파수>` | 하나의 Qualcomm DDR 주파수를 시험 종료까지 유지 |
| 전체 주파수 sweep | `--ddr-freq all` | 등록된 전체 주파수를 순서대로 반복 |
| 선택 주파수 sweep | `--ddr-freq <목록>` | 쉼표로 지정한 주파수만 입력 순서대로 반복 |
| 전환 간격 | `--ddr-step <초>` | sweep 주파수 유지 시간. 기본값은 3초 |
| AOSS node 변경 | `--ddr-node <경로>` | 기본 debugfs node가 다른 제품에서 사용 |
| 오류 주파수 기록 | 자동 | expected data의 마지막 write, 최초 mismatch read, `Flush()` 호출 후 reread 시점의 값을 각각 출력 |

기본 Qualcomm AOSS node는 다음 경로입니다.

```text
/sys/kernel/debug/aoss_send_message
```

프로그램은 shell의 `echo`를 실행하지 않고 다음 메시지를 debugfs node에 직접 씁니다.

```text
{class:ddr, res:fixed, val:3196}
```

테스트가 끝난 후 마지막 fixed DDR 요청을 별도로 해제하지 않습니다.

## 선택 가능한 옵션 전체 정리

아래 표는 이 저장소의 `Sat::ParseArgs()`가 실제로 인식하는 옵션을 기준으로 작성했습니다. `-P`와 DDR 관련 옵션은 이 fork에서 추가했으며 나머지는 기존 stressapptest 옵션입니다.

### 메모리와 실행 시간

| 옵션 | 기본값 | 설명 |
|---|---:|---|
| `-M <MiB>` | 자동 | 시험에 사용할 메모리 크기입니다. Android에서는 LMKD와 system process를 고려하여 직접 지정하는 것이 안전합니다. |
| `--reserve_memory <MiB>` | 0 | `-M`을 생략했을 때 운영체제에 남길 메모리입니다. 실제 parser는 하이픈이 아니라 밑줄을 사용합니다. |
| `-H <MiB>` | 0 | 시험에 필요한 최소 huge page 용량입니다. |
| `-s <초>` | 20 | Worker가 동작하는 시험 시간입니다. 초기 pattern 기록 시간과 마지막 정리 시간은 포함하지 않습니다. |
| `-p <byte>` | 1 MiB | stressapptest가 queue에서 관리하는 한 memory block의 크기입니다. 1,024 byte 이상의 2의 거듭제곱이어야 합니다. |
| `-m <개수>` | online CPU 수 | 메모리를 읽고 다른 block에 복사하면서 checksum을 계산하는 Copy Worker 수입니다. |
| `-i <개수>` | 0 | block 데이터를 반전하여 읽기·쓰기와 cache 관리 동작을 반복하는 Invert Worker 수입니다. |
| `-c <개수>` | 0 | block을 복사하지 않고 checksum과 pattern을 검사하는 Check Worker 수입니다. |
| `-C <개수>` | 0 | 부동소수점 연산으로 CPU 부하를 추가하는 CPU Stress Worker 수입니다. |
| `-W` | 사용 안 함 | AArch64에서는 NEON load/store와 checksum을 사용하는 CPU 부하가 큰 복사 경로를 선택합니다. |
| `-F` | 사용 안 함 | transaction마다 수행하는 checksum 검사를 생략하고 일반 `memcpy()` 경로를 사용합니다. |
| `-A` | 사용 안 함 | 완전히 지원되지 않는 환경에서도 제한된 기능으로 실행을 허용합니다. |
| `--coarse_grain_lock` | 사용 안 함 | 기본 block별 lock 대신 queue 전체를 하나의 lock으로 관리합니다. Queue 구현 비교용 옵션입니다. |
| `--random-threads <개수>` | 0 | 각 raw disk write thread에 추가할 random disk thread 수입니다. |

### Pattern과 DDR 제어

| 옵션 | 기본값 | 설명 |
|---|---:|---|
| `-P <ID\|이름[,ID\|이름...]>` | 가중치 기반 무작위 | 지정한 pattern을 입력 순서대로 block에 순환 배정합니다. |
| `--ddr-freq <주파수>` | 사용 안 함 | 값 하나를 AOSS fixed DDR 값으로 사용합니다. |
| `--ddr-freq <a,b,...>` | 사용 안 함 | 지정한 값만 입력 순서대로 sweep합니다. |
| `--ddr-freq all` | 사용 안 함 | 이 fork에 등록된 전체 DDR 값 목록을 순서대로 sweep합니다. |
| `--ddr-step <초>` | 3 | 다음 DDR 값으로 전환하기까지의 시간입니다. `--ddr-freq`가 없으면 단독으로 동작하지 않습니다. |
| `--ddr-node <경로>` | `/sys/kernel/debug/aoss_send_message` | Qualcomm AOSS control node 경로를 변경합니다. |

### 로그와 오류 처리

| 옵션 | 기본값 | 설명 |
|---|---:|---|
| `-l <파일>` | stdout만 사용 | 같은 로그를 지정한 파일과 stdout에 기록합니다. |
| `-v <0-20>` | 8 | 로그 상세도를 지정합니다. 숫자가 클수록 더 많은 로그가 출력됩니다. |
| `--printsec <초>` | 10 | 남은 실행 시간을 출력하는 간격입니다. |
| `--no_timestamps` | 사용 안 함 | 각 로그 앞의 wall-clock timestamp를 제거합니다. |
| `--max_errors <개수>` | 제한 없음 | Main control loop에서 전체 오류 수가 지정값을 초과하면 시험을 조기 종료합니다. 즉시 종료가 아니라 확인 주기만큼 지연될 수 있습니다. |
| `--stop_on_errors` | 사용 안 함 | 첫 오류 정지를 위한 기존 옵션입니다. 현재 일반 RAM miscompare 경로에서는 모든 Worker의 즉시 종료를 보장하지 않습니다. 철자는 반드시 `errors`입니다. |
| `--no_errors` | 사용 안 함 | ECC 등 운영체제 오류 polling을 비활성화합니다. Pattern 비교 자체를 끄는 옵션은 아닙니다. |
| `--force_errors` | 사용 안 함 | 오류 처리 경로 점검을 위해 인위적인 데이터 오류를 삽입합니다. 제품 판정 시험에는 사용하지 않습니다. |
| `--force_errors_like_crazy` | 사용 안 함 | 다량의 인위적 오류를 반복 삽입합니다. 로그와 오류 처리 시험용입니다. |
| `--monitor_mode` | 사용 안 함 | 메모리 부하 없이 ECC 등 오류 상태만 polling합니다. |

### Cache, CPU 배치와 주소 분석

| 옵션 | 기본값 | 설명 |
|---|---:|---|
| `--cc_test` | 사용 안 함 | 여러 Worker가 공유 cache line을 갱신하는 cache coherency 시험을 추가합니다. |
| `--cc_inc_count <개수>` | 1000 | Cache coherency Worker가 공유 값을 증가시키는 횟수입니다. |
| `--cc_line_count <개수>` | 2 | Cache coherency 시험에 사용하는 cache-line 크기 객체 수입니다. |
| `--cc_line_size <byte>` | 자동 | 자동 검출한 cache line 크기를 덮어씁니다. |
| `--no_affinity` | 사용 안 함 | stressapptest 내부 CPU affinity 설정을 비활성화합니다. 외부 `taskset`과 함께 사용할 때 유용합니다. |
| `--local_numa` | 사용 안 함 | Worker가 실행되는 NUMA node의 메모리를 우선 사용합니다. |
| `--remote_numa` | 사용 안 함 | Worker와 다른 NUMA node의 메모리를 우선 사용합니다. 일반적인 모바일 SoC에서는 NUMA 정보가 제공되지 않을 수 있습니다. |
| `--tag_mode` | 사용 안 함 | 각 cache line의 첫 8 byte에 virtual address 기반 tag를 기록하여 주소 전달 오류를 검사합니다. |
| `--do_page_map` | 사용 안 함 | 시험에서 접근한 physical page 범위를 출력합니다. Android 권한 정책에 따라 physical address를 얻지 못할 수 있습니다. |
| `--paddr_base <주소>` | 0 | 지원되는 memory allocator에서 사용할 physical base address를 지정합니다. 일반 Android anonymous memory에서는 그대로 적용되지 않을 수 있습니다. |
| `--pause_delay <초>` | 600 | Worker를 일시 정지하여 power spike를 만드는 주기입니다. |
| `--pause_duration <초>` | 15 | 각 pause 상태를 유지하는 시간입니다. |
| `--cpu_freq_test` | 사용 안 함 | CPU clock 측정 시험을 추가합니다. DDR sweep 기능과는 별도입니다. |
| `--cpu_freq_threshold <MHz>` | 0 | CPU frequency가 이 값보다 낮으면 실패 처리합니다. `--cpu_freq_test`를 사용할 때 0보다 큰 값이 필요합니다. |
| `--cpu_freq_round <MHz>` | 10 | 측정한 CPU frequency를 반올림하는 단위입니다. |
| `--channel_hash <mask>` | `0x40` | Physical address를 memory channel로 해석할 때 사용하는 address bit mask입니다. |
| `--channel_width <bit>` | 64 | Memory channel 폭을 지정합니다. |
| `--memory_channel <이름,...>` | 사용 안 함 | Channel에 속한 module 이름을 지정합니다. Vendor mapping 정보가 없으면 LPDDR bank·row 위치를 자동으로 계산하지 못합니다. |

### File, raw disk와 network 시험

| 옵션 | 기본값 | 설명 |
|---|---:|---|
| `--findfiles` | 사용 안 함 | File I/O 시험에 사용할 임시 파일 위치를 자동 탐색합니다. |
| `-f <파일>` | 사용 안 함 | 지정한 임시 파일을 사용하는 File Worker를 하나 추가합니다. 여러 번 지정할 수 있습니다. |
| `--filesize <byte>` | 8 MiB | File Worker가 사용할 임시 파일 크기입니다. |
| `-d <device>` | 사용 안 함 | 지정한 block device 또는 파일에 raw disk Worker를 추가합니다. 기본 non-destructive 상태에서는 읽기만 수행합니다. |
| `--destructive` | 사용 안 함 | `-d` 대상에 데이터를 기록합니다. Partition 데이터가 삭제될 수 있으므로 시험 전 대상을 반드시 확인해야 합니다. |
| `--read-block-size <byte>` | 512 | Raw disk read block 크기입니다. |
| `--write-block-size <byte>` | read 크기 | Raw disk write block 크기입니다. |
| `--segment-size <byte>` | 자동 | Raw disk 공간을 나누는 segment 크기입니다. |
| `--cache-size <byte>` | 자동 | Disk cache 크기 추정값을 지정합니다. |
| `--blocks-per-segment <개수>` | 자동 | 한 번의 반복에서 segment마다 처리할 block 수입니다. |
| `--read-threshold <µs>` | 제한 없음 | Disk read 시간이 이 값을 초과하면 성능 경고를 출력합니다. |
| `--write-threshold <µs>` | 제한 없음 | Disk write 시간이 이 값을 초과하면 성능 경고를 출력합니다. |
| `-n <IP 또는 host>` | 사용 안 함 | 원격 stressapptest listener와 page를 송수신하는 Network Worker를 추가합니다. |
| `--listen` | 사용 안 함 | 다른 stressapptest Network Worker의 연결을 받는 listener를 실행합니다. |
| `-h`, `--help` | 해당 없음 | 프로그램 도움말을 출력하고 종료합니다. |

Android 모바일 DRAM 시험의 기본 구성에서는 주로 `-M`, `-s`, `-m`, `-i`, `-c`, `-P`, `--printsec`, `--max_errors`, `--ddr-freq`, `--ddr-step`을 사용합니다. File, raw disk와 network 옵션은 DRAM-only 시험에는 필요하지 않습니다.

## 휴대폰에 설치

GitHub Release에서 바이너리를 받은 다음 Android 기기에 복사합니다.

```bash
adb push stressapptest-android-arm64 /data/local/tmp/stressapptest
adb shell chmod 0755 /data/local/tmp/stressapptest
```

Qualcomm AOSS node를 변경하려면 root 권한과 허용된 SELinux domain이 필요합니다. `adb root`를 지원하는 `userdebug` 또는 `eng` build에서는 다음과 같이 실행할 수 있습니다.

```bash
adb root
adb shell
cd /data/local/tmp
```

상용 `user` build에서는 root 권한과 SELinux 정책이 별도로 준비되어야 합니다.

## 패턴 선택

패턴 번호는 0부터 시작합니다. 이 fork에서 ID `27`은 `OneZero256`입니다.

다음 두 명령은 동일하게 작동합니다.

```bash
./stressapptest \
  -M 1024 -m 4 -i 4 -s 600 \
  --printsec 10 \
  -P 27
```

```bash
./stressapptest \
  -M 1024 -m 4 -i 4 -s 600 \
  --printsec 10 \
  -P OneZero256
```

패턴 이름은 대소문자를 구분하지 않습니다. `-P`를 생략하면 upstream과 동일하게 가중치 기반 무작위 패턴 선택을 사용합니다.

lowercase `-p`는 기존 memory block 크기 옵션입니다. 패턴 선택에는 반드시 uppercase `-P`를 사용합니다.

여러 패턴은 쉼표로 연결합니다. 각 Worker가 새 block의 pattern을 선택할 때 목록의 다음 항목을 가져오며, 마지막 항목 다음에는 첫 항목으로 돌아갑니다.

```bash
./stressapptest \
  -M 1024 -m 4 -i 4 -s 600 \
  --printsec 10 \
  -P OneZero256,FiveA256,walkingOnes128
```

Pattern 선택 호출 순서는 보존됩니다. 여러 Fill Worker가 block을 병렬로 가져오므로 메모리 주소의 증가 순서와 pattern 순서는 일치하지 않을 수 있습니다. 이 기능은 각 pattern을 일정 시간씩 실행하는 단계 전환 방식이 아니라 block별 순환 배정 방식입니다.

## DDR 주파수 하나로 고정

OneZero256 패턴을 3196 요청값으로 10분 동안 실행합니다.

```bash
./stressapptest \
  -M 1024 -m 4 -i 4 -s 600 \
  --printsec 10 \
  -P OneZero256 \
  --ddr-freq 3196
```

## 전체 DDR 주파수 sweep

`all`에 등록된 값은 다음과 같습니다.

```text
547, 768, 1017, 1353, 1555, 1708, 2092, 2736, 3196, 4266, 5333
```

각 값을 3초씩 유지하면서 순서대로 반복합니다.

```bash
./stressapptest \
  -M 1024 -m 4 -i 4 -s 600 \
  --printsec 10 \
  -P OneZero256 \
  --ddr-freq all
```

## 선택한 DDR 주파수만 sweep

쉼표 사이에 공백을 넣지 않습니다.

```bash
./stressapptest \
  -M 1024 -m 4 -i 4 -s 600 \
  --printsec 10 \
  -P OneZero256 \
  --ddr-freq 547,1017,2092,3196,5333 \
  --ddr-step 3
```

`--ddr-step`은 선택 옵션이며 기본값은 3초입니다. `--ddr-freq`를 생략하면 DDR 제어 기능은 비활성화되고 AOSS node를 열거나 쓰지 않습니다. `--ddr-step`만 지정해도 DDR 주파수는 변경되지 않습니다.

## 로그 확인

주파수 값을 AOSS node에 성공적으로 쓰면 다음 로그가 기록됩니다. 초기 pattern 기록 전과 Worker 시작 전에 첫 값을 각각 쓰므로 같은 값이 두 번 표시됩니다.

```text
Log: DDR_FREQ write=3196 monotonic_us=... node=/sys/kernel/debug/aoss_send_message
```

Memory mismatch가 발생하면 세 시점의 DDR 값이 같은 오류 record에 저장됩니다.

```text
Hardware Error: miscompare on CPU 3(<-1) at ...: read:0x0000000000000000, reread:0xffffffff00000000 expected:0xffffffff00000000. 'OneZero256' read error. ddr_freq(write=3196 read=4266 reread=5333).
```

`write`는 해당 block의 expected data를 마지막으로 전체 기록하기 직전에 저장한 값, `read`는 mismatch가 발생한 최초 load 직전에 저장한 값, `reread`는 `Flush()` 호출 뒤 다시 load하기 직전에 저장한 값입니다. Sweep 전환이 이 동작 사이에 발생하면 세 값이 서로 다를 수 있습니다.

각 숫자는 AOSS node에 마지막으로 성공적으로 쓴 내부 기록값이며 실제 DDR clock readback 결과가 아닙니다. DDR 제어를 사용하지 않았거나 write metadata가 없는 경로는 `unknown`으로 표시됩니다. 제품의 clock readback node가 있다면 별도 계측값과 함께 확인해야 합니다.

최초 read 뒤에는 한 번의 `CheckRegion()`에서 최대 128개 오류를 먼저 수집하고 각 주소를 순차적으로 reread합니다. 따라서 오류 출력이 많으면 `read`와 `reread` 사이에 다음 3초 sweep 전환이 발생할 수 있습니다. Logger queue에서 출력이 지연되어도 세 값은 각 동작 시점에 이미 record에 저장되어 있습니다.

## Read와 reread를 수행하는 이유

일반 RAM 검사 경로는 다음 순서로 동작합니다.

```text
Pattern에 맞는 expected data를 cacheable memory에 기록
 → Copy 또는 Check Worker가 source data를 읽으면서 checksum 계산
 → checksum이 expected checksum과 다르면 CheckRegion() 실행
 → CheckRegion()이 64-bit word 단위로 actual 값을 다시 읽음
 → actual != expected인 주소를 ErrorRecord에 저장
 → ProcessError()가 해당 주소에 Flush() 호출
 → 같은 주소를 reread
 → read·reread·expected를 로그에 기록
 → 해당 주소에 expected 값을 다시 기록
```

Reread의 목적은 첫 번째 불일치가 같은 주소를 다시 읽었을 때도 유지되는지 확인하는 것입니다. 원래 설계 의도는 cache line을 정리한 뒤 재검사하여 일시적인 첫 read와 지속되는 잘못된 데이터를 구분하는 것입니다. 그러나 아래 AArch64 제한 때문에 Android ARM64에서는 이를 DRAM read와 DRAM write의 확정 판정으로 사용할 수 없습니다.

### 최초 read는 cache miss가 보장되지 않음

`CheckRegion()`은 최초 word read 전에 cache flush를 실행하지 않습니다. 이전 checksum 계산이나 다른 Worker의 접근으로 같은 line이 cache에 남아 있으면 cache hit가 발생할 수 있습니다. 반대로 working set이 cache보다 크고 다른 Worker가 계속 접근하면 자연스럽게 evict되어 cache miss가 발생할 수도 있습니다.

따라서 다음과 같이 이해해야 합니다.

```text
최초 read = 검사 시점에 CPU load가 관찰한 값
최초 read ≠ 항상 DRAM에서 직접 가져온 값
```

<sub><em>Cache hit: 요청한 cache line이 현재 cache에 있어 하위 cache나 DRAM 접근 없이 값을 반환하는 상태입니다.</em></sub>
<sub><em>Cache miss: 요청한 line이 현재 cache에 없어 하위 cache 또는 memory hierarchy에서 가져와야 하는 상태입니다.</em></sub>
<sub><em>Eviction: 다른 cache line을 배치하기 위해 기존 cache line을 cache에서 내보내는 동작입니다.</em></sub>

### Write와 read마다 cache line을 flush하는지

| 동작 경로 | 명시적인 cache 관리 | AArch64에서의 의미 |
|---|---|---|
| 초기 `FillPage()` pattern write | 없음 | 일반 cacheable store입니다. Dirty line은 coherency와 eviction에 따라 이후 write-back됩니다. |
| 기본 `CrcCopyPage()` | 없음 | Source를 읽고 destination에 쓰면서 checksum을 계산합니다. Cache hit·miss는 현재 cache 상태에 따라 결정됩니다. |
| `CheckRegion()` 최초 read | 없음 | CRC mismatch 뒤 64-bit word를 비교하지만 DRAM read를 강제하지 않습니다. |
| `InvertPageUp/Down()` | 각 처리 구간 뒤 `FastFlushHint()` 직접 호출 | AArch64 `FastFlush()`의 `dc cvau`를 실행하여 data cache line을 PoU까지 clean합니다. Data cache invalidate 명령은 아닙니다. |
| `ProcessError()` reread 직전 | `OsLayer::Flush()` 호출 | 현재 공개 AArch64 경로에서는 실제 cache 관리 명령이 실행되지 않습니다. |
| 오류 복구 expected write 뒤 | `OsLayer::Flush()` 호출 | 현재 공개 AArch64 경로에서는 실제 cache 관리 명령이 실행되지 않습니다. |

<sub><em>Write-back: CPU가 cache에 수정한 dirty line을 하위 cache 또는 memory hierarchy로 기록하는 동작입니다.</em></sub>
<sub><em>Clean: dirty cache line의 값을 지정된 coherency 지점까지 기록하지만 해당 line을 cache에서 반드시 무효화하지는 않는 동작입니다.</em></sub>
<sub><em>Invalidate: cache line을 유효하지 않은 상태로 바꾸어 다음 load가 해당 cache entry를 그대로 사용하지 못하게 하는 동작입니다.</em></sub>
<sub><em>PoU: Point of Unification의 약어이며 instruction fetch와 data access가 같은 memory 값을 관찰하도록 합쳐지는 지점입니다. DRAM 자체와 동일한 의미가 아닙니다.</em></sub>

### AArch64에서 `Flush()`가 실제로 동작하지 않는 이유

`OsLayer::Flush()`는 `has_clflush_`가 true일 때만 `FastFlush()`를 호출합니다.

```cpp
void OsLayer::Flush(void *vaddr) {
  if (has_clflush_) {
    OsLayer::FastFlush(vaddr);
  }
}
```

현재 `OsLayer::GetFeatures()`의 AArch64 분기는 NEON 사용 여부만 설정하고 `has_clflush_`는 false로 유지합니다. 따라서 Android ARM64에서 `ProcessError()`가 `Flush()` 함수를 호출하더라도 실제 `dc` cache 관리 명령은 실행되지 않습니다.

또한 AArch64 `FastFlush()` 구현 자체는 `dc cvau`를 사용합니다. 이 명령은 data cache line을 PoU까지 clean하지만 data cache line을 invalidate하지 않습니다. 그러므로 `FastFlush()`를 직접 호출하는 Invert 경로도 다음 load의 DRAM 접근을 항상 보장하지 않습니다.

Android ARM64에서 cache를 확실히 clean·invalidate하여 하위 memory access를 유도하려면 EL0 cache-maintenance 권한, SoC coherency 구조와 Point of Coherency를 고려한 별도 구현이 필요합니다. 일반적으로는 검증된 vendor kernel driver 또는 권한이 확인된 AArch64 cache-maintenance 경로를 사용해야 합니다.

### Read와 reread 결과 해석

| 결과 | stressapptest 동작 | 올바른 해석 |
|---|---|---|
| `read != expected`, `reread == expected` | 일반 miscompare 로그에 `read error` 문자열 추가 | 첫 번째 관찰만 일시적으로 잘못되었음을 의미합니다. Read path 또는 cache 관찰 문제의 근거가 될 수 있지만 DRAM read failure를 확정하지 않습니다. |
| `read == reread`, 두 값 모두 `expected`와 다름 | 같은 잘못된 값이 두 번 관찰됨 | Data가 검사 전에 잘못 기록되었거나 지속적으로 잘못 저장·전달되는 상태의 근거입니다. Write failure로 단정할 수 없습니다. |
| `read != reread`, 두 값 모두 `expected`와 다름 | 서로 다른 잘못된 값이 연속 관찰됨 | 반복 read 불안정, cache/coherency 변화 또는 진행 중인 data corruption을 검토해야 합니다. 단일 read/write 분류가 불가능합니다. |
| `read == expected` | ErrorRecord를 만들지 않음 | 정상 비교이므로 reread를 수행하지 않습니다. |

따라서 `read == reread`이면 무조건 write failure이고 `read != reread`이면 무조건 read failure라는 해석은 정확하지 않습니다. 현재 Android ARM64 구현에서는 reread 전에 data cache invalidate가 보장되지 않기 때문에 이 구분은 더욱 제한적입니다. 가장 강한 software 근거는 `reread == expected`일 때 첫 번째 관찰만 일시적으로 달랐다는 사실까지입니다.

## 직접 Android ARM64 빌드

Android NDK 경로를 지정하고 실행합니다. 기본 Android API level은 30입니다.

```bash
export ANDROID_NDK_HOME=/path/to/android-ndk
./scripts/build_android_arm64.sh 30
```

결과 파일은 다음 경로에 생성됩니다.

```text
out/android-arm64/stressapptest
```

빌드 스크립트는 `aarch64-linux-android` target, PIE, pthread, `STRESSAPPTEST_CPU_AARCH64`를 사용합니다. C++ runtime은 실행 파일에 정적으로 연결하므로 휴대폰에 `libc++_shared.so`를 별도로 복사하지 않습니다.

## GitHub Actions 자동 빌드와 Release

`.github/workflows/android-arm64-release.yml`은 다음 순서로 실행됩니다.

1. PR에서 Android AArch64 빌드 가능 여부를 확인합니다.
2. `master`에 소스 변경이 반영되면 해당 commit을 Android API 30용으로 빌드합니다.
3. ELF architecture가 AArch64인지 확인합니다.
4. `libc++_shared.so` 의존성이 없는지 확인합니다.
5. source commit으로 `android-<commit>` tag와 GitHub Release를 만듭니다.
6. 실행 파일, SHA-256 checksum, 압축 파일을 Release 자산으로 등록합니다.
7. README 상단 다운로드 버튼이 가장 최신 Release의 실행 파일을 내려받습니다.

GitHub Actions 화면에서 `Build Android ARM64 release` workflow를 수동 실행할 수도 있습니다. Release는 `master` commit에서 실행했을 때만 게시됩니다.

## 상세 설명서

기존 소스 구조와 worker, queue, cache, physical mapping 설명은 [StressAppTest 설명 사이트](https://stpcoder.github.io/stressapptest/)에서 확인할 수 있습니다.

License: Apache License 2.0

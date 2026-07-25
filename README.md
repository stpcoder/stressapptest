# StressAppTest Android ARM64 확장판

이 저장소는 공개 [stressapptest](https://github.com/stressapptest/stressapptest)에 Android AArch64 실장기 테스트 기능을 추가한 개인 fork입니다. 기본 branch인 `master`에서 소스, 한글 매뉴얼과 Android ARM64 배포 파일을 함께 관리합니다.

[![Android ARM64 최신 버전 다운로드](https://img.shields.io/badge/Android_ARM64-최신_버전_다운로드-1976d2?style=for-the-badge&logo=android&logoColor=white)](https://github.com/stpcoder/stressapptest/releases/latest/download/stressapptest-android-arm64)
[![최신 Release](https://img.shields.io/github/v/release/stpcoder/stressapptest?display_name=tag&style=for-the-badge&label=Latest%20Release&color=455a64)](https://github.com/stpcoder/stressapptest/releases/latest)
[![한글 매뉴얼](https://img.shields.io/badge/한글_매뉴얼-바로_보기-0288d1?style=for-the-badge&logo=materialformkdocs&logoColor=white)](https://stpcoder.github.io/stressapptest/)

첫 번째 버튼은 Android ARM64 실행 파일을 직접 다운로드합니다. 최신 Release 페이지에는 실행 파일, 압축 파일과 SHA-256 checksum이 함께 제공됩니다. 한글 매뉴얼 버튼은 Worker, queue, cache, memory access와 오류 로그 설명으로 연결됩니다.

## 바로 사용하기

```bash
# PC에서 다운로드한 실행 파일을 Android 기기로 복사
adb push stressapptest-android-arm64 /data/local/tmp/stressapptest
adb shell chmod 0755 /data/local/tmp/stressapptest

# 기본 memory stress 실행
adb shell '/data/local/tmp/stressapptest -M 512 -s 60 -m 4 -v 8'
```

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

프로그램은 다음 메시지를 debugfs node에 `write()`로 전달합니다.

```text
{class:ddr, res:fixed, val:3196}
```

테스트 종료 후에도 마지막 fixed DDR 요청이 유지됩니다.

## 선택 가능한 옵션 전체 정리

아래 표는 이 저장소의 `Sat::ParseArgs()`가 실제로 인식하는 옵션을 기준으로 작성했습니다. `-P`와 DDR 관련 옵션은 이 fork에서 추가했으며 나머지는 기존 stressapptest 옵션입니다.

### 메모리와 실행 시간

| 옵션 | 기본값 | 설명 |
|---|---:|---|
| `-M <MiB>` | 자동 | 시험에 사용할 메모리 크기입니다. Android에서는 LMKD와 system process를 고려하여 직접 지정하는 것이 안전합니다. |
| `--reserve_memory <MiB>` | 0 | `-M` 자동 계산에서 운영체제용으로 남길 메모리입니다. 옵션 이름에는 밑줄을 사용합니다. |
| `-H <MiB>` | 0 | 시험에 필요한 최소 huge page 용량입니다. |
| `-s <초>` | 20 | Worker가 동작하는 시험 시간입니다. 초기 pattern 기록과 마지막 정리는 이 시간의 앞뒤에서 실행됩니다. |
| `-p <byte>` | 1 MiB | stressapptest가 queue에서 관리하는 한 memory block의 크기입니다. 1,024 byte 이상의 2의 거듭제곱이어야 합니다. |
| `-m <개수>` | online CPU 수 | 메모리를 읽고 다른 block에 복사하면서 checksum을 계산하는 Copy Worker 수입니다. |
| `-i <개수>` | 0 | block 데이터를 반전하여 읽기·쓰기와 cache 관리 동작을 반복하는 Invert Worker 수입니다. |
| `-c <개수>` | 0 | Block의 checksum과 pattern을 검사하는 Check Worker 수입니다. |
| `-C <개수>` | 0 | 부동소수점 연산으로 CPU 부하를 추가하는 CPU Stress Worker 수입니다. |
| `-W` | 사용 안 함 | AArch64에서는 NEON load/store와 checksum을 사용하는 CPU 부하가 큰 복사 경로를 선택합니다. |
| `-F` | 사용 안 함 | transaction마다 수행하는 checksum 검사를 생략하고 일반 `memcpy()` 경로를 사용합니다. |
| `-A` | 사용 안 함 | 일부 호환성 검사를 완화하여 제한된 기능으로 실행합니다. |
| `--coarse_grain_lock` | 사용 안 함 | Queue 전체를 하나의 lock으로 관리합니다. Queue lock 방식 비교에 사용합니다. |
| `--random-threads <개수>` | 0 | 각 raw disk write thread에 추가할 random disk thread 수입니다. |

### Pattern과 DDR 제어

| 옵션 | 기본값 | 설명 |
|---|---:|---|
| `-P <ID\|이름[,ID\|이름...]>` | 가중치 기반 무작위 | 지정한 pattern을 입력 순서대로 block에 순환 배정합니다. |
| `--ddr-freq <주파수>` | 사용 안 함 | 값 하나를 AOSS fixed DDR 값으로 사용합니다. |
| `--ddr-freq <a,b,...>` | 사용 안 함 | 지정한 값만 입력 순서대로 sweep합니다. |
| `--ddr-freq all` | 사용 안 함 | 이 fork에 등록된 전체 DDR 값 목록을 순서대로 sweep합니다. |
| `--ddr-step <초>` | 3 | DDR sweep에서 각 값을 유지하는 시간입니다. `--ddr-freq`와 함께 사용합니다. |
| `--ddr-node <경로>` | `/sys/kernel/debug/aoss_send_message` | Qualcomm AOSS control node 경로를 변경합니다. |

### 로그와 오류 처리

| 옵션 | 기본값 | 설명 |
|---|---:|---|
| `-l <파일>` | stdout만 사용 | 같은 로그를 지정한 파일과 stdout에 기록합니다. |
| `-v <0-20>` | 8 | 로그 상세도를 지정합니다. 숫자가 클수록 더 많은 로그가 출력됩니다. |
| `--printsec <초>` | 10 | 남은 실행 시간을 출력하는 간격입니다. |
| `--no_timestamps` | 사용 안 함 | 각 로그 앞의 wall-clock timestamp를 제거합니다. |
| `--max_errors <개수>` | 제한 없음 | 전체 오류 수가 지정값을 초과하면 main control loop가 종료 절차를 시작합니다. 확인 주기에 따라 종료 시점이 결정됩니다. |
| `--stop_on_errors` | 사용 안 함 | 첫 오류 이후 종료 절차를 요청합니다. 일반 RAM miscompare는 Worker별 처리 중인 작업을 마친 뒤 정리됩니다. 옵션 철자는 `errors`입니다. |
| `--no_errors` | 사용 안 함 | 운영체제 오류를 확인하는 `ErrorPollThread`를 중지합니다. Pattern 비교는 계속 실행됩니다. |
| `--force_errors` | 사용 안 함 | 오류 처리 경로 점검용 데이터 오류를 삽입합니다. 시험 환경 검증에 사용합니다. |
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
| `--remote_numa` | 사용 안 함 | Worker와 다른 NUMA node의 메모리를 우선 사용합니다. NUMA 정보를 제공하는 target에서 적용됩니다. |
| `--tag_mode` | 사용 안 함 | 각 cache line의 첫 8 byte에 virtual address 기반 tag를 기록하여 주소 전달 오류를 검사합니다. |
| `--do_page_map` | 사용 안 함 | 시험에서 접근한 physical page 범위를 출력합니다. `/proc/self/pagemap` 접근이 허용된 Android 환경에서 주소를 표시합니다. |
| `--paddr_base <주소>` | 0 | Physical base 지정 기능을 제공하는 memory allocator에서 시작 주소를 설정합니다. 공통 Android allocator는 anonymous memory를 사용합니다. |
| `--pause_delay <초>` | 600 | Worker를 일시 정지하여 power spike를 만드는 주기입니다. |
| `--pause_duration <초>` | 15 | 각 pause 상태를 유지하는 시간입니다. |
| `--cpu_freq_test` | 사용 안 함 | CPU clock 측정 시험을 추가합니다. DDR sweep 기능과는 별도입니다. |
| `--cpu_freq_threshold <MHz>` | 0 | CPU frequency가 이 값보다 낮으면 실패 처리합니다. `--cpu_freq_test`를 사용할 때 0보다 큰 값이 필요합니다. |
| `--cpu_freq_round <MHz>` | 10 | 측정한 CPU frequency를 반올림하는 단위입니다. |
| `--channel_hash <mask>` | `0x40` | Physical address를 memory channel로 해석할 때 사용하는 address bit mask입니다. |
| `--channel_width <bit>` | 64 | Memory channel 폭을 지정합니다. |
| `--memory_channel <이름,...>` | 사용 안 함 | Channel별 module 이름을 지정합니다. LPDDR bank·row 계산에는 vendor address map이 필요합니다. |

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

Android 모바일 DRAM 시험은 주로 `-M`, `-s`, `-m`, `-i`, `-c`, `-P`, `--printsec`, `--max_errors`, `--ddr-freq`, `--ddr-step`으로 구성합니다. File, raw disk와 network 옵션은 각 장치의 I/O 시험에 사용합니다.

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

패턴 이름은 대소문자를 동일하게 처리합니다. `-P`의 기본 동작은 upstream의 가중치 기반 무작위 선택입니다.

lowercase `-p`는 기존 memory block 크기 옵션입니다. 패턴 선택에는 반드시 uppercase `-P`를 사용합니다.

여러 패턴은 쉼표로 연결합니다. 각 Worker가 새 block의 pattern을 선택할 때 목록의 다음 항목을 가져오며, 마지막 항목 다음에는 첫 항목으로 돌아갑니다.

```bash
./stressapptest \
  -M 1024 -m 4 -i 4 -s 600 \
  --printsec 10 \
  -P OneZero256,FiveA256,walkingOnes128
```

Pattern은 입력 목록의 순서대로 선택됩니다. 여러 Fill Worker가 block을 병렬로 처리하므로 주소별 배치 순서는 실행 시점에 결정됩니다. `-P`는 pattern 목록을 block 단위로 순환 배정합니다.

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

주파수 값은 공백 없이 쉼표로 연결합니다.

```bash
./stressapptest \
  -M 1024 -m 4 -i 4 -s 600 \
  --printsec 10 \
  -P OneZero256 \
  --ddr-freq 547,1017,2092,3196,5333 \
  --ddr-step 3
```

`--ddr-step`의 기본값은 3초입니다. DDR 제어는 `--ddr-freq`를 지정한 실행에서 활성화됩니다. `--ddr-step`은 해당 주파수 목록의 전환 간격을 설정합니다.

## 로그 확인

주파수 값을 AOSS node에 성공적으로 쓰면 다음 로그가 기록됩니다. 초기 pattern 기록 전과 Worker 시작 전에 첫 값을 각각 쓰므로 같은 값이 두 번 표시됩니다.

```text
Log: DDR_FREQ write=3196 monotonic_us=... node=/sys/kernel/debug/aoss_send_message
```

Memory mismatch가 발생하면 세 시점의 DDR 값이 같은 오류 record에 저장됩니다.

```text
Hardware Error: miscompare on CPU 3(<-1) at ...: read:0x0000000000000000, reread:0xffffffff00000000 expected:0xffffffff00000000. 'OneZero256' read error. ddr_freq(write=3196 read=4266 reread=5333).
```

세 값은 각 memory 동작 직전의 DDR 설정값입니다.

| 필드 | 저장 시점 |
|---|---|
| `write` | Block의 expected data를 마지막으로 전체 기록하기 직전 |
| `read` | `CheckRegion()`의 mismatch load 직전 |
| `reread` | `Flush()` 호출 후 두 번째 load 직전 |

Sweep 전환이 세 동작 사이에 발생하면 각 필드에 서로 다른 값이 저장됩니다.

각 숫자는 AOSS node의 `write()`에 성공한 설정값입니다. Hardware DDR clock은 제품의 readback node 또는 hardware counter로 측정합니다. DDR metadata가 없는 record에는 `unknown`이 표시됩니다.

`CheckRegion()`은 최대 128개 오류를 수집한 후 각 주소를 순차적으로 reread합니다. 이 처리 중 sweep 전환이 발생할 수 있습니다. 각 주파수 값은 memory 동작 시점에 record에 저장되며 Logger 대기 중에도 유지됩니다.

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

Reread는 같은 주소의 두 번째 관찰값을 수집합니다. `read`와 `reread`의 관계를 사용하여 mismatch의 지속 여부를 확인합니다. Android ARM64에서는 아래 cache 관리 조건을 함께 적용하여 결과를 해석합니다.

### 최초 read의 cache 경로

`CheckRegion()`의 word read에는 사전 cache flush가 없습니다. 같은 line이 cache에 있으면 cache hit로 처리됩니다. Working set 증가와 다른 Worker의 접근으로 line이 evict된 상태에서는 cache miss가 발생합니다.

최초 read의 의미는 다음과 같습니다.

```text
최초 read = 검사 시점의 CPU load가 관찰한 값
응답 계층 = 현재 cache 상태와 coherency 동작에 따라 결정
```

<sub><em>Cache hit: 요청한 cache line이 현재 cache에 있어 하위 cache나 DRAM 접근 없이 값을 반환하는 상태입니다.</em></sub>
<sub><em>Cache miss: 요청한 line이 현재 cache에 없어 하위 cache 또는 memory hierarchy에서 가져와야 하는 상태입니다.</em></sub>
<sub><em>Eviction: 다른 cache line을 배치하기 위해 기존 cache line을 cache에서 내보내는 동작입니다.</em></sub>

### 동작별 cache 관리

| 동작 경로 | 명시적인 cache 관리 | AArch64에서의 의미 |
|---|---|---|
| 초기 `FillPage()` pattern write | 없음 | 일반 cacheable store입니다. Dirty line은 coherency와 eviction에 따라 이후 write-back됩니다. |
| 기본 `CrcCopyPage()` | 없음 | Source를 읽고 destination에 쓰면서 checksum을 계산합니다. Cache hit·miss는 현재 cache 상태에 따라 결정됩니다. |
| `CheckRegion()` 최초 read | 없음 | CRC mismatch 구간을 64-bit 단위로 비교합니다. 응답 계층은 현재 cache 상태에 따라 결정됩니다. |
| `InvertPageUp/Down()` | 각 처리 구간 뒤 `FastFlushHint()` 호출 | AArch64 `dc cvau`가 data cache line을 PoU까지 clean합니다. Line의 valid 상태는 유지될 수 있습니다. |
| `ProcessError()` reread 직전 | `OsLayer::Flush()` 호출 | 공개 AArch64 경로의 `has_clflush_` 값이 `false`이므로 함수가 바로 반환합니다. |
| 오류 복구 expected write 뒤 | `OsLayer::Flush()` 호출 | 공개 AArch64 경로의 `has_clflush_` 값이 `false`이므로 함수가 바로 반환합니다. |

<sub><em>Write-back: CPU가 cache에 수정한 dirty line을 하위 cache 또는 memory hierarchy로 기록하는 동작입니다.</em></sub>
<sub><em>Clean: Dirty cache line의 값을 지정된 coherency 지점까지 기록하는 동작입니다. Cache line의 valid 상태는 유지될 수 있습니다.</em></sub>
<sub><em>Invalidate: Cache line을 invalid 상태로 변경하여 다음 load가 하위 memory 계층에서 값을 요청하게 하는 동작입니다.</em></sub>
<sub><em>PoU: Point of Unification의 약어이며 instruction fetch와 data access가 같은 값을 관찰하도록 합쳐지는 지점입니다.</em></sub>

### AArch64 `Flush()` 실행 조건

`OsLayer::Flush()`는 `has_clflush_`가 true일 때만 `FastFlush()`를 호출합니다.

```cpp
void OsLayer::Flush(void *vaddr) {
  if (has_clflush_) {
    OsLayer::FastFlush(vaddr);
  }
}
```

현재 `OsLayer::GetFeatures()`의 AArch64 분기는 `has_clflush_`를 `false`로 유지합니다. Android ARM64에서 `ProcessError()`의 `Flush()`는 조건문을 확인한 뒤 반환합니다.

AArch64 `FastFlush()`는 `dc cvau`를 사용하여 data cache line을 PoU까지 clean합니다. Line은 valid 상태로 유지될 수 있습니다. Invert 경로의 다음 load 응답 계층은 해당 시점의 cache 상태에 따라 결정됩니다.

Android ARM64의 강제 clean·invalidate 기능은 EL0 cache-maintenance 권한, SoC coherency 구조와 Point of Coherency를 반영한 경로로 구현합니다. 제품 시험에는 검증된 vendor kernel driver 또는 권한이 확인된 AArch64 cache-maintenance 경로를 사용합니다.

### Read와 reread 결과 해석

| 결과 | stressapptest 동작 | 올바른 해석 |
|---|---|---|
| `read != expected`, `reread == expected` | 일반 miscompare 로그에 `read error` 문자열 추가 | 첫 상세 검사와 두 번째 load 사이에서 관찰값이 expected로 변경됐습니다. CPU, cache, coherency와 read data path를 확인합니다. |
| `read == reread`, 두 값 모두 `expected`와 다름 | 같은 mismatch 값이 두 번 관찰됨 | Mismatch가 두 번의 load 동안 유지됐습니다. 마지막 write 이후의 데이터 상태와 반복 재현성을 확인합니다. |
| `read != reread`, 두 값 모두 `expected`와 다름 | 서로 다른 mismatch 값이 연속 관찰됨 | 두 load 사이에서 관찰값이 변경됐습니다. CPU migration, cache 상태와 transient data path를 확인합니다. |
| `read == expected` | 정상 비교로 처리 | 현재 word 검사를 종료하고 다음 word로 이동합니다. |

이 표는 두 load에서 CPU가 관찰한 값의 관계를 나타냅니다. 공개 Android ARM64 경로의 reread는 현재 cache 상태를 유지한 CPU load입니다. Hardware 발생 위치는 CPU·cache PMU, interconnect, memory controller, PHY, DRAM과 RAS 정보를 함께 사용하여 판정합니다.

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

빌드 스크립트는 `aarch64-linux-android` target, PIE, pthread, `STRESSAPPTEST_CPU_AARCH64`를 사용합니다. C++ runtime은 실행 파일에 정적으로 포함됩니다.

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

---

## StressAppTest 기본 설명과 분석 문서

이 저장소는 stressapptest의 메모리 준비, Worker 실행, cache 동작과 데이터 검사 과정을 공개 소스 기준으로 설명합니다. 분석 기준은 upstream commit `73b9df227e89cd52b09852056843610722b7b7ae`입니다.

### 핵심 동작

- 기본 설정에서는 online 상태의 논리 CPU 수만큼 `CopyThread`를 만듭니다. `-m N`으로 개수를 변경할 수 있습니다.
- 테스트 메모리는 기본 1 MiB 크기의 SAT block으로 나뉩니다. Worker는 공용 queue에서 읽을 block과 쓸 block을 가져옵니다.
- 한 block 내부에서는 주소 순서대로 읽고 씁니다. 다음 block은 여러 block 중에서 선택합니다.
- 기본 복사 과정은 원본 block read, checksum 계산과 대상 block write를 함께 수행합니다.
- stressapptest는 cache가 활성화된 일반 메모리를 사용합니다. 여러 core와 큰 working set을 사용하면 cache miss와 write-back이 반복됩니다.
- 함수 이름에는 `CRC`가 사용되지만 실제 검증에는 modified Adler checksum을 사용합니다.
- 프로그램의 처리량은 Worker가 처리한 논리적 byte입니다. 실제 LPDDR transaction은 DMC·NoC·SLC 계측값으로 확인합니다.

<sub><em>Worker: 특정 memory·CPU·I/O 부하 또는 검증 loop를 실행하는 pthread 단위입니다.</em></sub><br>
<sub><em>Write-back: 수정된 cache line을 하위 cache 또는 system memory 방향으로 기록하는 동작입니다.</em></sub><br>
<sub><em>Physical mapping: virtual address를 system physical address에 대응시키는 변환 관계입니다.</em></sub>

### 한글 문서 목차

처음 확인할 때는 다음 순서로 읽을 수 있습니다. 같은 내용은 [`docs/README.md`](docs/README.md)와 [배포된 한글 매뉴얼](https://stpcoder.github.io/stressapptest/)에서 제공합니다.

1. [stressapptest의 작동 원리](docs/01-overview.md)
2. [실행 순서 한눈에 보기](docs/02-execution-flow.md)
3. [메모리를 복사하고 오류를 찾는 과정](docs/09-copy-and-verification.md)
4. [Cache에서 LPDDR까지 데이터가 이동하는 과정](docs/04-cache-and-arm64.md)
5. [메모리 Worker 종류와 동작](docs/07-memory-workers.md)
6. [목적에 따른 테스트 명령](docs/12-test-recipes.md)
7. [부하와 오류를 측정하는 방법](docs/13-measurement.md)
8. [오류 로그 처리 과정과 DRAM 주파수 기록](docs/17-logging-and-dram-frequency.md)

사이트 메뉴는 [`mkdocs.yml`](mkdocs.yml), 본문 style은 [`docs/stylesheets/extra.css`](docs/stylesheets/extra.css)에서 관리합니다.

### 문서 사이트를 로컬에서 확인하기

```bash
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r requirements-docs.txt
mkdocs serve
```

브라우저에서 `http://127.0.0.1:8000/stressapptest/`를 열어 배포 전 화면을 확인할 수 있습니다.

### 일반 Linux 빌드

```bash
./configure
make -j"$(nproc)"
./src/stressapptest --help
```

### Memory access별 실행 명령

```bash
# Read와 checksum 검사
stressapptest -M 512 -s 60 -m 0 -c 4

# Read, write와 checksum을 수행하는 기본 copy
stressapptest -M 512 -s 60 -m 4

# libc memcpy 처리량 확인
stressapptest -M 512 -s 60 -m 4 -F

# Read-Modify-Write와 접근 방향 전환
stressapptest -M 512 -s 60 -m 0 -i 4

# CPU 계산 부하 병행
stressapptest -M 512 -s 60 -m 4 -C 4
```

`-d ... --destructive`는 지정한 block device를 덮어씁니다. 데이터 삭제가 허용된 시험용 장치에만 사용합니다.

### Upstream과 라이선스

- Upstream: <https://github.com/stressapptest/stressapptest>
- 원본 README: <https://github.com/stressapptest/stressapptest/blob/73b9df227e89cd52b09852056843610722b7b7ae/README.md>
- License: Apache License 2.0. 기존 `COPYING`과 `NOTICE`를 유지합니다.

Physical address를 LPDDR channel, rank, bank, row와 column으로 변환하거나 DMC counter를 해석할 때는 target platform의 memory-controller 자료를 함께 사용합니다.

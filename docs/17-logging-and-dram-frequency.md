# 로그 출력과 DRAM 주파수 전환 오류를 분석하는 방법

이 장에서는 stressapptest가 메모리 오류 로그를 만드는 과정과 DRAM 주파수 변경 기록을 같은 시간축에서 분석하는 방법을 설명합니다. Public upstream의 Logger 구조와 이 fork에 추가한 Qualcomm DDR 제어 기능을 구분하여 설명합니다.

## 이 fork의 통합 DDR 로그

외부 script 대신 다음 옵션을 사용하면 stressapptest가 Qualcomm AOSS node에 직접 fixed DDR 값을 전달합니다.

```bash
# 한 주파수 고정
stressapptest -M 1024 -m 4 -i 4 -s 600 \
  -P OneZero256 --ddr-freq 3196

# 전체 주파수 3초 sweep
stressapptest -M 1024 -m 4 -i 4 -s 600 \
  -P OneZero256 --ddr-freq all
```

`--ddr-freq` 뒤에 값 하나를 쓰면 고정하고, `all` 또는 쉼표 목록을 쓰면 순서대로 sweep합니다. `--ddr-step`은 선택 옵션이며 기본값은 3초입니다. `--ddr-freq`를 생략하면 AOSS node에 접근하지 않습니다.

주파수 값을 AOSS node에 성공적으로 쓰면 다음 로그를 기록합니다.

```text
Log: DDR_FREQ write=3196 monotonic_us=... node=/sys/kernel/debug/aoss_send_message
```

`write`는 전체 메시지 쓰기가 성공한 직후에 기록됩니다. Hardware readback이 아니라 프로그램이 마지막으로 성공적으로 쓴 값입니다.

오류 record는 expected data의 마지막 whole-block write, 첫 mismatching read, cache flush 뒤의 reread 시점 값을 각각 저장합니다.

```text
Hardware Error: ... read:0x..., reread:0x... expected:0x.... 'OneZero256' read error. ddr_freq(write=3196 read=4266 reread=5333).
```

| 필드 | 저장 시점 |
|---|---|
| `write` | 해당 block을 pattern fill, copy, invert, file read 또는 network receive로 마지막 전체 기록하기 직전 |
| `read` | `CheckRegion()`이 잘못된 actual 값을 읽기 직전 |
| `reread` | `ProcessError()`가 해당 주소의 cache flush를 호출한 뒤 다시 읽기 직전 |

Pattern fill과 최초 read 사이에 여러 sweep 전환이 발생할 수 있으므로 `write`와 `read`가 달라질 수 있습니다. `CheckRegion()`은 한 번에 최대 128개의 오류 record를 저장한 뒤 각 주소를 순차 처리하므로, 오류가 많으면 최초 read와 reread 사이에도 다음 sweep 전환이 발생할 수 있습니다.

세 값은 각 동작 시점에 record에 저장되므로 Logger queue에서 출력이 지연되어도 출력 시점의 값으로 바뀌지 않습니다. `unknown`은 DDR 옵션을 사용하지 않았거나 해당 write 경로의 metadata가 없다는 뜻입니다.

이 값은 실제 DDR clock 측정값이 아닙니다. 프로그램이 AOSS node에 성공적으로 쓴 마지막 값을 기록합니다. 제품에 readback node가 있으면 같은 시간축의 측정값과 함께 확인해야 합니다.

## 먼저 구분해야 하는 시각

DRAM 주파수를 일정 간격으로 변경하는 시험에서는 다음 시각이 서로 다를 수 있습니다.

```text
주파수 변경 command를 기록한 시각
 → kernel driver가 command를 받은 시각
 → clock·전압·timing 전환이 완료된 시각
 → 메모리 데이터가 실제로 변경된 시각
 → Worker가 해당 데이터를 다시 읽은 시각
 → stressapptest가 오류 로그를 queue에 넣은 시각
 → Logger thread가 파일과 stdout에 기록한 시각
 → adb 또는 terminal 화면에 표시된 시각
```

로그에 표시된 시각은 stressapptest가 불일치를 검출하여 `logprintf()`를 호출한 시각입니다. DRAM에서 데이터가 처음 변경된 시각을 직접 나타내지 않습니다. 대상 block에 오류가 발생한 뒤 해당 block이 다시 원본이나 검사 대상으로 선택될 때까지 검출이 지연될 수도 있습니다.

<sub><em>Detection latency: 데이터가 실제로 변경된 시점부터 software 검사가 불일치를 발견할 때까지의 시간입니다.</em></sub>
<sub><em>Settling interval: clock, voltage 및 memory timing 전환 요청 이후 hardware 상태가 안정되도록 구분한 시간 구간입니다.</em></sub>

## 로그 출력 구조

Stressapptest는 Worker가 로그를 직접 파일이나 terminal에 기록하지 않습니다. Worker가 만든 로그를 공용 queue에 넣고 전용 Logger thread가 출력합니다.

```text
Worker의 logprintf()
 → Logger::VLogF()
 → timestamp와 문자열 생성
 → queued_lines_에 삽입
 → Logger thread가 queue를 가져감
 → 지정한 logfile에 write()
 → STDOUT_FILENO에 write()
```

> **파일:** `src/logger.cc` · **함수:** `Logger::VLogF()` · **기준:** `73b9df2`

```cpp
void Logger::VLogF(int priority, const char *format, va_list args) {
  if (priority > verbosity_) {
    return;
  }
  char buffer[4096];
  size_t length = 0;
  if (log_timestamps_) {
    time_t raw_time;
    time(&raw_time);
    struct tm time_struct;
    localtime_r(&raw_time, &time_struct);
    length = strftime(buffer, sizeof(buffer),
                      "%Y/%m/%d-%H:%M:%S(%Z) ", &time_struct);
  }
  length += vsnprintf(buffer + length, sizeof(buffer) - length,
                      format, args);
  QueueLogLine(new string(buffer, length));
}
```

**코드 설명:** timestamp는 Logger thread가 출력할 때가 아니라 Worker가 `VLogF()`를 호출할 때 생성됩니다. 단위는 초이며 local timezone을 사용합니다. 이후 메시지는 queue에서 대기할 수 있습니다.

`Logger::ThreadMain()`은 queue의 메시지를 local queue로 옮긴 뒤 공용 mutex를 풀고 순서대로 출력합니다.

> **파일:** `src/logger.cc` · **함수:** `Logger::WriteAndDeleteLogLine()` · **기준:** `73b9df2`

```cpp
if (log_fd_ >= 0) {
  bytes_written = write(log_fd_, line->data(), line->size());
}
bytes_written = write(STDOUT_FILENO, line->data(), line->size());
```

**코드 설명:** C library의 `printf()` buffering을 사용하지 않고 file descriptor에 `write()`합니다. `-l`로 logfile을 지정하면 같은 메시지를 logfile에 먼저 쓰고 stdout에도 씁니다. Logfile 쓰기가 지연되면 stdout 표시도 같이 늦어질 수 있습니다.

### Queue가 로그 시각에 미치는 영향

Logger queue의 제한은 250개입니다.

```cpp
static const size_t kMaxQueueSize = 250;
```

Queue가 가득 차면 Worker는 새 로그를 넣을 수 있을 때까지 기다립니다. 오류가 집중되면 다음 조건이 발생할 수 있습니다.

- 여러 Worker가 만든 로그가 짧은 시간에 연속 출력됨
- 로그 timestamp와 terminal 표시 시각 사이의 지연 증가
- 로그 queue가 가득 차서 오류 처리 Worker가 대기
- Logger thread와 adb·UFS 처리가 CPU, scheduler, NoC에 추가 부하 생성
- 갑작스러운 reboot에서 아직 queue에 남은 마지막 로그가 유실

정상 종료에서는 `Logger::StopThread()`가 queue에 남은 메시지를 모두 출력한 뒤 Logger thread를 종료합니다. Watchdog reset, kernel panic 또는 전원 재시작에서는 이 정리 과정이 실행되지 않을 수 있습니다.

### `-l` logfile의 동기 쓰기

`Sat::InitializeLogfile()`은 platform이 지원하면 `O_DSYNC`, `O_SYNC` 또는 `O_FSYNC`를 사용하여 logfile을 엽니다.

> **파일:** `src/sat.cc` · **함수:** `Sat::InitializeLogfile()` · **기준:** `73b9df2`

```cpp
logfile_ = open(logfilename_,
#if defined(O_DSYNC)
                O_DSYNC |
#elif defined(O_SYNC)
                O_SYNC |
#elif defined(O_FSYNC)
                O_FSYNC |
#endif
                O_WRONLY | O_CREAT, mode);
```

**코드 설명:** Logger thread가 logfile에 전달한 메시지는 일반적인 지연 쓰기보다 보존성이 높습니다. 그러나 각 로그 쓰기가 UFS I/O와 전력·NoC 부하를 추가할 수 있고, Logger queue에만 있던 메시지는 보호하지 못합니다.

| 기록 방식 | 장점 | 확인할 제한사항 |
|---|---|---|
| `-l /data/local/tmp/sat.log` | 갑작스러운 종료 직전 로그 보존 가능성 증가 | 동기 UFS 쓰기가 시험 부하에 추가됨 |
| stdout을 host에서 저장 | 단말 logfile 쓰기를 줄일 수 있음 | adb 연결 중단이나 reboot에서 마지막 출력 유실 가능 |

로그 보존 시험과 순수 메모리 부하 비교 시험을 분리하여 두 기록 방식이 재현 결과에 영향을 주는지 확인합니다.

## 메모리 불일치가 로그로 확대되는 과정

기본 복사 방식에서는 4 KiB마다 계산한 checksum이 기대값과 다르면 `CheckRegion()`이 64-bit word 단위로 다시 검사합니다.

```text
4 KiB checksum 불일치
 → CheckRegion() slow compare
 → actual과 expected를 64-bit word별로 비교
 → ErrorRecord 저장
 → ProcessError()에서 reread와 주소 정보 확인
 → Report Error와 상세 miscompare 출력
 → 잘못된 word를 expected 값으로 복구
```

> **파일:** `src/worker.cc` · **함수:** `WorkerThread::CheckRegion()` · **기준:** `73b9df2`

```cpp
const int kErrorLimit = 128;
struct ErrorRecord recorded[kErrorLimit];

if (actual != expected) {
  if (errors < kErrorLimit) {
    recorded[errors].actual = actual;
    recorded[errors].expected = expected;
    recorded[errors].vaddr = &memblock[i];
    recorded[errors].patternname = pattern->name();
    recorded[errors].lastcpu = lastcpu;
    errors++;
  } else {
    page_error = true;
    break;
  }
}
```

**코드 설명:** 한 번의 `CheckRegion()` 호출은 우선 최대 128개의 오류 record를 저장합니다. 기본 checksum 경로에서는 일반적으로 4 KiB 구간을 대상으로 호출됩니다. 넓은 데이터 손상 하나가 여러 64-bit word의 불일치로 기록될 수 있습니다.

각 Worker의 초기 상세 오류는 다음 조건으로 출력합니다.

```cpp
int priority = 5;
if (errorcount_ + err < 30)
  priority = 0;
ProcessError(&recorded[err], priority, errormessage.c_str());
```

`ProcessError()`는 `priority < 5`인 오류에 대해 parse 가능한 `Report Error`와 상세 miscompare를 기록합니다. 여러 Worker가 동시에 오류를 발견하면 Worker별 초기 오류가 각각 출력되어 로그가 집중될 수 있습니다.

```text
로그에 나온 miscompare word 수 ≠ 독립적인 DRAM failure 발생 횟수
```

첫 오류로 넓은 영역이 변경되었는지, 여러 시간대에서 독립 오류가 반복되었는지는 timestamp, 주소 범위, XOR bit 차이와 Worker 정보를 함께 사용하여 구분합니다.

## `Report Error` 원시 메시지 해석

상세 miscompare를 출력하기 전에 `ErrorDiag::AddMiscompareError()`가 `OsLayer::ErrorReport()`를 호출합니다.

```text
Report Error: miscompare : DIMM Unknown : 1 : 42s
```

형식은 다음과 같습니다.

```text
Report Error: symptom : part : count : time-to-failure
```

| 필드 | 의미 |
|---|---|
| `miscompare` | 검출한 오류 종류 |
| `DIMM Unknown` | 공통 `FindDimm()`이 계산한 위치 문자열. Android에서 실제 LPDDR 위치를 의미하지 않을 수 있음 |
| `1` | 해당 `ErrorReport()` 호출에서 보고한 개수 |
| `42s` | `OsLayer` 초기화 이후 경과한 초 |

마지막 `42s`는 Logger의 wall-clock timestamp와 별도로 생성되는 상대 시간입니다. DRAM frequency script도 시험 시작 기준의 uptime을 기록하면 상대 시간 정합성을 추가로 확인할 수 있습니다.

## 상세 miscompare 필드 해석

다음은 pattern name과 마지막 writer CPU가 포함된 형식의 예입니다.

```text
Hardware Error: miscompare on CPU 6(<-3)
at 0x7abc0000(0x12340000:DIMM Unknown):
read:0x00000000ffffffff,
reread:0xffffffff00000000
expected:0xffffffff00000000. 'OneZero128' read error.
ddr_freq(write=3196 read=4266 reread=5333).
```

| 필드 | 의미 |
|---|---|
| `CPU 6` | 상세 검사를 수행한 시점의 CPU |
| `<-3` | 프로그램이 해당 block을 마지막으로 쓴 CPU로 기록한 값 |
| 첫 번째 주소 | Process virtual address |
| 괄호 안 주소 | 변환 가능한 경우 system physical address |
| `read` | slow compare에서 처음 읽은 actual 값 |
| `reread` | 오류 처리 중 다시 읽은 값 |
| `expected` | pattern에서 계산한 기대값 |
| `OneZero128` | 검사한 block에 지정된 기대 pattern |
| `read error` | `reread == expected`일 때 추가되는 분류 문자열 |
| `ddr_freq(write=...)` | expected data를 해당 block에 마지막으로 전체 기록하기 직전의 내부 DDR 값 |
| `ddr_freq(read=...)` | 최초 mismatch load 직전의 내부 DDR 값 |
| `ddr_freq(reread=...)` | cache flush 호출 뒤 reread load 직전의 내부 DDR 값 |

`lastcpu`는 software가 기록한 마지막 writer CPU 후보입니다. CPU migration, vendor 수정, DMA와 다른 device의 접근을 포함하는 hardware trace가 아니므로 보조 정보로 사용합니다.

Physical address가 `0`, 제한된 값 또는 `DIMM Unknown`으로 표시되면 `/proc/self/pagemap` 권한과 vendor address mapping 구현을 확인해야 합니다. 이 상태에서는 로그의 위치 문자열을 실제 LPDDR channel·bank·row로 해석하지 않습니다.

## `read`, `reread`, `expected`를 비교하는 방법

`ProcessError()`는 불일치가 발견된 주소에 대해 다음 순서로 처리합니다.

```text
최초 mismatch load 직전에 read 주파수 저장
 → ErrorRecord에 actual·expected·write·read 저장
 → OsLayer::Flush(vaddr)
 → reread 주파수 저장
 → 같은 주소를 reread
 → physical address와 위치 문자열 계산
 → 오류 로그 기록
 → expected 값을 해당 주소에 다시 기록
 → OsLayer::Flush(vaddr)
```

| 비교 결과 | 코드에서 확인되는 상태 | 해석 시 제한사항 |
|---|---|---|
| `read != expected`, `reread == expected` | 첫 번째 읽기에서만 불일치하여 `read error` 표시 | 일시적 read path, cache 관찰 또는 재검사 시점 변화 가능 |
| `read != expected`, `reread != expected` | 다시 읽어도 데이터 불일치 유지 | 잘못된 write, 저장 상태 변경 또는 반복 read 오류 가능 |
| 같은 주소·같은 XOR bit 반복 | 위치 또는 data path 의존성 검토 대상 | VA 재사용과 PA 신뢰도 확인 필요 |
| 넓은 연속 주소의 동시 불일치 | 하나의 광범위한 손상이 여러 word 오류로 분해되었을 가능성 | 로그 줄 수를 독립 failure 수로 사용하지 않음 |

공통 AArch64 구현에서는 `has_clflush_`가 false인 경우 `OsLayer::Flush()`가 실제 cache 관리 명령을 실행하지 않습니다. 따라서 `read error` 문자열만으로 DRAM read failure와 write failure를 확정하지 않습니다.

오류 출력 후에는 `expected` 값을 다시 기록합니다. 이후 같은 위치가 정상으로 보이더라도 오류가 자연적으로 사라졌다고 판단하면 안 됩니다.

## `OneZero` pattern 오류를 해석하는 방법

`OneZero`의 기본 32-bit 데이터는 다음 두 값의 반복입니다.

> **파일:** `src/pattern.cc` · **구간:** `OneZero` · **기준:** `73b9df2`

```cpp
static unsigned int OneZero_data[] = {
  0x00000000, 0xffffffff
};

static const struct PatternData OneZero = {
  "OneZero",
  OneZero_data,
  1,
  {5, 5, 15, 5}
};
```

Pattern 이름은 반복 범위와 반전 여부를 포함합니다.

```text
OneZero32, OneZero64, OneZero128, OneZero256
OneZero~32, OneZero~64, OneZero~128, OneZero~256
```

전체 pattern weight 합은 원본과 반전 variant를 포함하여 160입니다. OneZero 계열의 합은 60이므로 전체 선택 확률은 37.5%입니다.

| OneZero 범위 | 원본과 반전 variant를 합한 선택 비율 |
|---|---:|
| 32-bit | 6.25% |
| 64-bit | 6.25% |
| 128-bit | 18.75% |
| 256-bit | 6.25% |
| 전체 | 37.50% |

로그의 `'OneZero128'`은 검사한 block의 기대 pattern이 OneZero128이었다는 뜻입니다. 다음 내용을 직접 증명하지 않습니다.

- OneZero 데이터가 failure의 원인이라는 결론
- OneZero를 처음 기록한 순간에 failure가 발생했다는 결론
- 상세 로그가 표시된 DRAM 주파수에서 failure가 발생했다는 결론
- 실제 LPDDR DQ pin에 `0x00000000`과 `0xffffffff`가 그대로 교대했다는 결론

OneZero는 다른 pattern보다 자주 선택되므로 단순 오류 건수 대신 노출 횟수로 정규화한 오류율을 비교합니다.

```text
OneZero 오류 word 수 / OneZero 처리 transaction 수
다른 pattern 오류 word 수 / 해당 pattern 처리 transaction 수
```

기본 로그에는 pattern별 전체 처리 transaction 수가 없습니다. 정확한 비교에는 `CrcCopyPage()`와 `CrcCheckPage()` 진입 시 pattern별 counter를 추가하거나, pattern weight를 변경하여 특정 pattern만 사용하는 별도 binary가 필요합니다.

## 3초 주파수 전환 로그를 기록하는 방법

DRAM frequency 변경 interface는 AOSP 공통 규격이 아닙니다. Qualcomm, MediaTek 및 단말 제조사마다 control node, command 형식, 단위와 readback 의미가 다릅니다. 다음 예제의 경로와 command는 target의 vendor 문서에 맞게 변경합니다.

주파수 변경 script는 최소한 다음 세 상태를 기록합니다.

```text
COMMAND: 변경 command를 쓰기 직전
WRITE_DONE: control node의 write가 반환된 직후
READBACK: 실제 또는 요청 상태를 읽은 결과
```

Control node의 `write()` 성공은 command가 접수되었다는 의미입니다. Clock, voltage와 memory timing 전환 완료를 자동으로 보장하지 않습니다. Readback node가 요청값을 반환하는지 실제 hardware frequency를 반환하는지도 vendor 정의로 확인합니다.

```sh
#!/system/bin/sh

CONTROL_NODE=/sys/path/to/vendor_dram_control
READBACK_NODE=/sys/path/to/vendor_dram_current_freq
FREQ_LOG=/data/local/tmp/dram_frequency.log

stamp() {
    wall=$(date '+%Y/%m/%d-%H:%M:%S%z')
    uptime=$(cut -d' ' -f1 /proc/uptime)
    printf '%s uptime=%s' "$wall" "$uptime"
}

apply_frequency() {
    freq="$1"

    printf '%s COMMAND freq=%s\n' \
        "$(stamp)" "$freq" >> "$FREQ_LOG"

    # Target의 실제 vendor command 형식으로 교체합니다.
    printf 'stat fix freq %s\n' "$freq" > "$CONTROL_NODE"

    write_rc="$?"
    printf '%s WRITE_DONE freq=%s rc=%s\n' \
        "$(stamp)" "$freq" "$write_rc" >> "$FREQ_LOG"

    actual=$(cat "$READBACK_NODE" 2>/dev/null)
    printf '%s READBACK selected=%s value=%s\n' \
        "$(stamp)" "$freq" "$actual" >> "$FREQ_LOG"
}

while true
do
    for freq in 1737 1536 1353 1737
    do
        apply_frequency "$freq"
        sleep 3
    done
done
```

Stressapptest와 frequency script에서 다음 조건을 동일하게 유지합니다.

- Wall-clock timezone
- 시험 시작 시각
- `/proc/uptime` 기준
- Frequency 값의 단위
- Readback 값의 의미
- Script cycle과 frequency 순서

Stressapptest 기본 timestamp는 초 단위입니다. 3초 전환 시험에서는 1초 해상도, detection latency와 hardware settling 시간이 겹치므로 경계 부근 failure의 주파수를 하나로 확정하기 어렵습니다.

정확한 시간 정렬이 필요하면 `Logger::VLogF()`에 `clock_gettime(CLOCK_MONOTONIC)` 기반 microsecond timestamp를 추가하고 frequency script도 같은 monotonic clock을 기록합니다.

## 주파수 구간에 failure를 배치하는 방법

각 주파수 구간을 다음 상태로 구분합니다.

```text
이전 주파수 안정 구간
 → COMMAND
 → WRITE_DONE
 → READBACK
 → settling 구간
 → 현재 주파수 안정 구간
 → 다음 COMMAND
```

오류 timestamp가 `COMMAND` 전후 또는 settling 구간에 있으면 `transition-associated`로 분류합니다. 현재 주파수가 충분히 안정된 구간 안에 있는 경우에만 해당 고정 주파수와의 연관성을 평가합니다.

3초 간격이 hardware 전환 시간과 software detection latency에 비해 짧으면 failure가 어느 주파수에서 생성되었는지 분리하기 어렵습니다. 다음 두 시험을 별도로 수행합니다.

### 고정 주파수 시험

- 각 frequency를 충분한 시간 동안 고정
- 같은 stressapptest 명령과 시작 온도 사용
- Frequency별 반복 횟수 확보
- 고정 구간 중간에서 발생한 failure만 우선 비교
- Frequency 순서를 변경하여 온도와 시간 경과 영향을 분리

고정 주파수에서 반복 failure가 발생하면 해당 OPP의 안정성, voltage, timing 및 temperature 조건을 검토합니다.

### 주파수 전환 시험

- 기존 3초 cycle을 별도 시험으로 유지
- COMMAND·READBACK·settling 구간 기록
- 낮은 주파수→높은 주파수와 높은 주파수→낮은 주파수 구분
- 동일한 전환 방향에서 failure가 반복되는지 확인
- 고정 주파수에서는 통과하고 전환 구간에서만 실패하는지 확인

고정 주파수에서는 통과하고 transition window에서만 반복 실패하면 절대 frequency보다 DVFS 전환 과정과의 연관성을 우선 검토합니다. 이 결과만으로 특정 clock, voltage 또는 timing 회로를 원인으로 확정하지는 않습니다.

## 첫 failure를 보존하는 방법

분석 우선순위는 다음과 같습니다.

1. 첫 번째 `Report Error`
2. 첫 번째 상세 miscompare
3. 같은 4 KiB 범위의 후속 오류
4. 다른 Worker와 주소에서 발생한 독립 오류
5. 최종 오류 집계와 `Status: FAIL`

첫 failure 시점에 다음 상태를 함께 저장합니다.

```text
요청 DRAM frequency
DRAM frequency readback
마지막 frequency 전환 방향과 경과 시간
DRAM·SoC·PMIC·CPU temperature
DMC read/write counter
CPU affinity와 현재 CPU
kernel/RAS/SError/thermal 로그
stressapptest read/reread/expected와 pattern
virtual address와 확인 가능한 physical address
```

### `--max_errors`의 제한

`--max_errors N`은 주 실행 thread가 전체 오류 수를 확인한 뒤 종료합니다. 현재 소스에서는 5초 단위 control loop에서 다음 조건을 사용합니다.

```cpp
if (errors > max_errorcount_) {
  break;
}
```

따라서 `--max_errors 1`은 첫 오류에서 즉시 중단하지 않습니다. 오류 수가 1을 초과해야 하며, 다음 control loop 확인 전까지 여러 Worker가 추가 오류를 출력할 수 있습니다.

현재 public 코드의 `--stop_on_errors`도 일반 RAM miscompare 경로에서 즉시 전체 Worker를 종료하는 기능으로 사용하기 어렵습니다. File sector 오류 경로에는 명시적인 종료가 있지만 `CheckRegion()`의 일반 메모리 불일치에는 같은 종료 호출이 없습니다.

첫 RAM 오류 직후 중단하려면 다음 방법 중 하나를 사용합니다.

- 외부 watcher가 첫 `Report Error: miscompare`를 확인하고 SAT process에 `SIGINT` 전달
- `CheckRegion()`이 첫 오류를 기록한 후 전체 Worker stop 상태 설정
- `ProcessError()`에서 frequency·thermal·DMC 상태를 직접 snapshot

외부 watcher도 Logger queue에 기록된 이후에 동작하므로 실제 오류 발생 시점과 차이가 있습니다. 가장 정확한 방법은 오류 검출 코드 내부에서 monotonic timestamp와 target 상태를 함께 저장하는 것입니다.

## 로그 양이 시험 조건에 미치는 영향

기본 verbosity는 8입니다. `-v 0`을 사용하면 일반 정보 로그는 줄지만 priority 0인 초기 상세 오류는 계속 출력됩니다.

대량 로그가 발생하면 다음 부하가 추가됩니다.

- Logger thread의 문자열 생성과 mutex 처리
- Logfile의 동기 UFS 쓰기
- stdout과 adbd·USB 전송
- Worker의 queue 대기
- 오류 주소의 reread, physical mapping 조회와 기대값 복구 write

따라서 오류 발생 후의 bandwidth, power와 temperature는 정상 부하 구간과 직접 비교하지 않습니다. 첫 오류 이전 구간과 이후 오류 처리 구간을 분리하여 분석합니다.

## 오류 형태별 확인 항목

| 관찰 결과 | 우선 확인할 내용 |
|---|---|
| 고정 1737에서 반복 failure | 해당 OPP의 voltage·timing·temperature와 재현성 |
| 1737 진입 직후만 failure | 상승 전환 과정, settling 시간, readback 의미 |
| 1737 이탈 직후만 failure | 하강 전환 과정과 voltage·clock 순서 |
| OneZero 로그 비율이 높음 | Pattern별 transaction 노출 횟수로 정규화 |
| OneZero128만 반복 | 128-bit 반복 variant의 노출률과 다른 variant 비교 |
| 동일 주소·동일 XOR bit 반복 | PA 신뢰도, 위치 재현성, address/data path |
| 연속 주소의 다수 word 불일치 | 하나의 광범위한 손상이 로그 여러 줄로 분해되었는지 확인 |
| `reread == expected` | 일시적 관찰 가능성. ARM64 Flush 조건을 함께 확인 |
| `reread != expected` | 지속 데이터 불일치. 오류 후 복구 write가 수행됨을 고려 |
| Reboot로 로그가 중단 | pstore, reboot reason, watchdog, SError, thermal 상태 확인 |

## 최소 분석 표

실행별로 다음 표를 작성하면 frequency와 pattern의 연관성을 비교할 수 있습니다.

| 항목 | 기록값 |
|---|---|
| Run ID |  |
| Binary build ID·commit |  |
| Stressapptest 명령 |  |
| Worker 수·CPU affinity |  |
| Frequency cycle |  |
| REQUEST 시각 |  |
| READBACK 시각·값 |  |
| 첫 Report Error 시각 |  |
| Transition 이후 경과 시간 |  |
| Pattern |  |
| CPU와 last writer CPU |  |
| VA·PA |  |
| Read |  |
| Reread |  |
| Expected |  |
| `read XOR expected`와 bit 수 |  |
| 같은 4 KiB 범위의 오류 word 수 |  |
| Temperature |  |
| DMC counter |  |
| Kernel·RAS 로그 |  |
| 종료 원인 |  |

이 표에서 frequency별 failure 횟수, transition 방향별 failure 횟수와 pattern별 정규화 오류율을 각각 계산합니다. 한 종류의 집계값만으로 DRAM 원인을 확정하지 않습니다.

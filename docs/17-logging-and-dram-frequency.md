# 오류 로그 생성 과정과 DRAM 주파수 기록

이 장에서는 memory Worker가 데이터를 검사하고 오류 로그를 출력하는 순서를 소스 코드의 공통 동작을 기준으로 설명합니다.

## 전체 처리 순서

Memory Worker의 검사와 로그 출력은 다음 순서로 진행됩니다.

```text
Fill Worker가 pattern 기록
 → Valid queue에 block 저장
 → Copy·Invert·Check Worker가 block 처리
 → 4 KiB 단위 checksum 검사
 → checksum mismatch 발생
 → 64-bit 단위 slow compare
 → ErrorRecord 저장
 → 같은 주소 reread
 → 주소와 DDR 주파수 정보 정리
 → Logger queue에 오류 메시지 저장
 → Logger thread가 logfile과 stdout에 출력
 → 잘못된 word를 expected 값으로 복구
```

`read`, `reread`, `expected`가 포함된 상세 로그는 위 과정에서 실제 mismatch가 검출됐을 때 생성됩니다. 정상적인 memory access는 이 형식으로 출력하지 않습니다.

## Worker별 검사 위치

| Worker | 주요 함수 | 검사 내용 |
|---|---|---|
| Fill Worker | `FillPage()` | Empty queue에서 받은 block에 expected pattern을 기록합니다. |
| Copy Worker | `CrcCopyPage()` | source를 destination으로 복사하면서 source checksum을 계산합니다. |
| Warm Copy Worker | `CrcWarmCopyPage()` | CPU 연산 부하를 추가한 copy와 checksum 검사를 수행합니다. |
| Invert Worker | `InvertPageUp()`, `InvertPageDown()` | block 값을 순방향과 역방향으로 반전합니다. Strict 검사에서는 처리 전후에 checksum을 확인합니다. |
| Check Worker | `CrcCheckPage()` | block 내용을 변경하지 않고 checksum과 expected pattern을 확인합니다. |

각 Worker는 queue에서 받은 block을 처리합니다. `page_entry`에는 memory address, expected pattern, 마지막 writer CPU와 마지막 전체 기록 시점의 DDR 설정값이 저장됩니다.

> **소스 위치:** `src/worker.cc`의 `FillPage()`, `CrcCopyPage()`, `CrcWarmCopyPage()`, `InvertThread::Work()`, `CrcCheckPage()`

## 1단계: checksum으로 빠르게 검사

Strict copy와 check 경로는 4 KiB 구간의 Adler checksum을 계산합니다.

```text
Memory block read
 → 4 KiB checksum 계산
 → expected checksum과 비교
```

Checksum이 같으면 다음 block을 처리합니다. Checksum이 다르면 `CheckRegion()`을 호출하여 어느 64-bit word가 다른지 확인합니다.

```cpp
if (!crc.Equals(*expectedcrc)) {
  CheckRegion(...);
}
```

Checksum 계산 중에는 block 전체를 읽습니다. Copy Worker는 source를 읽으면서 destination에도 데이터를 기록합니다.

> **소스 위치:** `src/worker.cc`의 `CrcCheckPage()`, `CrcCopyPage()`

## 2단계: 64-bit 단위로 mismatch 확인

`CheckRegion()`은 checksum mismatch가 발생한 구간을 64-bit 단위로 다시 읽습니다.

```text
actual = memory에서 읽은 64-bit 값
expected = pattern에서 계산한 64-bit 값
```

`actual != expected`이면 다음 정보를 `ErrorRecord`에 저장합니다.

- `actual`: slow compare에서 읽은 값
- `expected`: 해당 주소에 있어야 하는 값
- `vaddr`: mismatch가 발생한 virtual address
- `patternname`: expected 값을 계산한 pattern 이름
- `lastcpu`: block의 마지막 writer로 기록된 CPU
- `write_dram_frequency`: block을 마지막으로 전체 기록한 시점의 DDR 설정값
- `read_dram_frequency`: mismatch load 직전의 DDR 설정값

한 번의 `CheckRegion()`은 최대 128개의 `ErrorRecord`를 먼저 저장합니다. 저장이 끝나면 각 record를 `ProcessError()`로 전달합니다.

```cpp
const int kErrorLimit = 128;
struct ErrorRecord recorded[kErrorLimit];
```

상세 로그는 각 Worker의 초기 오류를 중심으로 출력합니다. 일반 RAM 경로에서는 Worker별 초기 약 30개 오류가 높은 우선순위로 출력되고, 이후 오류는 incident 수에 계속 포함됩니다.

> **소스 위치:** `src/worker.cc`의 `WorkerThread::CheckRegion()`

## 3단계: reread와 오류 정보 생성

`ProcessError()`는 저장된 mismatch를 다음 순서로 처리합니다.

```text
OsLayer::Flush(vaddr) 호출
 → reread 시점의 DDR 설정값 저장
 → 같은 virtual address를 다시 load
 → physical address 변환 시도
 → 위치 문자열 확인
 → 상세 mismatch 메시지 생성
 → expected 값을 해당 주소에 다시 기록
```

이때 두 번의 read 결과는 다음 필드에 저장됩니다.

```text
read   = CheckRegion()의 slow compare 결과
reread = ProcessError()에서 같은 주소를 다시 읽은 결과
```

`ProcessError()`는 상세 로그를 만든 뒤 해당 word에 `expected` 값을 다시 기록합니다. 이후 검사에서 같은 주소가 정상으로 확인될 수 있으므로 첫 오류 record를 우선 보존해야 합니다.

> **소스 위치:** `src/worker.cc`의 `WorkerThread::ProcessError()`

### AArch64의 `Flush()` 동작

현재 공개 AArch64 경로에서는 `has_clflush_`가 `false`로 유지됩니다. `ProcessError()`가 호출하는 `OsLayer::Flush()`는 `has_clflush_`가 `true`일 때만 cache 관리 명령을 실행합니다.

```cpp
void OsLayer::Flush(void *vaddr) {
  if (has_clflush_) {
    OsLayer::FastFlush(vaddr);
  }
}
```

따라서 현재 AArch64 일반 오류 처리에서 `reread`는 두 번째 CPU load입니다. 이 load가 L1, L2, system cache 또는 LPDDR 중 어느 계층에서 완료됐는지는 로그로 구분할 수 없습니다.

Invert Worker의 `FastFlushHint()`는 별도 경로입니다. AArch64 구현의 `DC CVAU`는 Data Cache line을 Point of Unification까지 clean합니다. Data Cache line은 valid 상태로 유지될 수 있으므로 이후 load의 LPDDR 접근 여부를 보장하지 않습니다.

> **소스 위치:** `src/os.cc`의 `OsLayer::GetFeatures()`, `OsLayer::Flush()`와 `src/os.h`의 `FastFlush()`, `FastFlushHint()`

## 4단계: Logger가 메시지 출력

Worker는 logfile과 terminal에 직접 쓰지 않습니다. 모든 메시지는 `logprintf()`를 통해 공용 Logger로 전달됩니다.

```text
Worker의 logprintf()
 → Logger::VLogF()
 → timestamp와 문자열 생성
 → queued_lines_에 저장
 → Logger thread가 queue를 가져감
 → -l logfile에 write()
 → stdout에 write()
```

`Logger::VLogF()`는 verbosity를 확인한 뒤 최대 4096-byte 문자열을 만듭니다. Timestamp 옵션을 사용하면 queue에 넣기 전에 wall-clock timestamp를 추가합니다.

Logger queue의 기본 제한은 250개 행입니다. Queue가 가득 차면 Worker는 공간이 생길 때까지 기다립니다. 정상 종료 시 `Logger::StopThread()`가 남아 있는 메시지를 모두 출력합니다.

```cpp
static const size_t kMaxQueueSize = 250;
```

`-l <file>`을 사용하면 같은 메시지가 logfile과 stdout에 기록됩니다. Logfile은 platform이 지원하는 동기 쓰기 flag로 열리므로 로그 보존성이 높아지고 storage I/O 부하가 추가됩니다.

> **소스 위치:** `src/sat.cc`의 `logprintf()`, `InitializeLogfile()`과 `src/logger.cc`의 `VLogF()`, `ThreadMain()`, `WriteAndDeleteLogLine()`

## 상세 mismatch 로그 읽기

일반 RAM mismatch는 다음 형식으로 출력됩니다.

```text
Hardware Error: miscompare on CPU <current_cpu>(<-<last_writer_cpu>)
at <virtual_address>(<physical_address>:<location>):
read:<actual>, reread:<second_actual> expected:<expected>.
'<pattern_name>' read error.
ddr_freq(write=<value> read=<value> reread=<value>).
```

### 오류 로그 예시

다음 내용은 로그 구조를 설명하기 위해 만든 가상 예시입니다. 실제 제품, pattern과 시험 결과를 사용하지 않았습니다.

```text
2026/01/15-10:20:00(KST) Log: DDR_FREQ write=3196 monotonic_us=120000000 node=<control_node>
2026/01/15-10:20:42(KST) Report Error: miscompare : DIMM Unknown : 1 : 42s
2026/01/15-10:20:42(KST) Hardware Error: miscompare on CPU 6(<-3) at 0x7a120000(0x12345000:DIMM Unknown): read:0x1122334455667780, reread:0x1122334455667788 expected:0x1122334455667788. '<selected_pattern>' read error. ddr_freq(write=3196 read=3196 reread=3196).
2026/01/15-10:20:45(KST) Log: Thread 4 found 1 hardware incidents
2026/01/15-10:20:45(KST) Stats: Found 1 hardware incidents
2026/01/15-10:20:45(KST) Status: FAIL - test discovered HW problems
```

이 예시는 다음 순서로 읽습니다.

1. `DDR_FREQ` 행에서 control node에 마지막으로 전달한 설정값과 monotonic 시각을 확인합니다.
2. `Report Error` 행에서 오류 종류와 시험 시작 이후 경과 시간을 확인합니다.
3. `Hardware Error` 행에서 주소, 두 번의 read 결과, expected와 각 시점의 DDR 설정값을 확인합니다.
4. `read`의 마지막 1 byte는 `0x80`, `expected`의 마지막 1 byte는 `0x88`입니다. `read XOR expected`는 `0x08`이므로 이 예시에서는 한 bit 차이가 기록됐습니다.
5. `reread == expected` 조건이 성립하여 `read error` 문자열이 추가됐습니다.
6. Worker와 전체 incident 집계가 1로 기록되고 최종 상태가 `FAIL`로 출력됐습니다.

이 예시에서 확인되는 결과는 첫 slow read의 mismatch와 정상값을 반환한 reread입니다. 발생 위치를 구분하려면 CPU, cache, memory controller, PHY와 DRAM의 hardware 정보를 추가로 확인합니다.

| 필드 | 의미 |
|---|---|
| `CPU <current_cpu>` | `ProcessError()`가 reread와 로그 생성을 수행한 시점의 CPU입니다. |
| `<-<last_writer_cpu>` | Software가 해당 block의 마지막 writer로 저장한 CPU입니다. |
| `virtual_address` | Process가 사용하는 virtual address입니다. |
| `physical_address` | `/proc/self/pagemap`으로 변환에 성공한 system physical address입니다. |
| `read` | `CheckRegion()` slow compare에서 처음 저장한 mismatch 값입니다. |
| `reread` | `ProcessError()`가 같은 주소에서 다시 읽은 값입니다. |
| `expected` | Pattern으로 계산한 기대값입니다. |
| `pattern_name` | 해당 block의 expected pattern 이름입니다. 원인 판정에는 반복 횟수와 다른 조건이 함께 필요합니다. |
| `read error` | `reread == expected`일 때 코드가 추가하는 분류 문자열입니다. |
| `ddr_freq(write=...)` | Block을 마지막으로 전체 기록한 시점의 내부 DDR 설정값입니다. |
| `ddr_freq(read=...)` | Slow compare가 mismatch 값을 읽기 직전의 내부 DDR 설정값입니다. |
| `ddr_freq(reread=...)` | `ProcessError()`가 reread하기 직전의 내부 DDR 설정값입니다. |

현재 로그의 `CPU` 필드는 최초 mismatch load를 실행한 CPU를 별도로 저장하지 않습니다. Worker가 두 load 사이에서 migration됐는지 확인하려면 `read_cpu`와 `reread_cpu`를 각각 기록하는 기능이 필요합니다.

`expected`는 pattern으로 계산합니다. 최초 write에서 실제 cache 또는 memory에 저장된 값을 별도로 읽어 보관하지는 않습니다.

Physical address가 `0` 또는 제한된 값으로 표시되면 `/proc/self/pagemap` 접근 권한을 확인합니다. System physical address를 LPDDR channel, rank, bank, row와 column으로 변환하려면 target의 memory-controller address mapping 정보가 필요합니다.

## `read`와 `reread` 해석

| 비교 결과 | 코드에서 확인되는 상태 | 추가 확인 범위 |
|---|---|---|
| `read != expected`, `reread == expected` | 첫 slow read에서 mismatch가 발생했고 두 번째 load는 expected를 반환했습니다. | CPU load, cache, coherency, interconnect, memory controller, PHY와 DRAM read 경로를 확인합니다. |
| `read == reread`, 두 값 모두 `expected`와 다름 | 같은 잘못된 값이 두 번 관찰됐습니다. | 마지막 write, 지속 데이터 변경과 반복 read 상태를 확인합니다. |
| `read != reread`, 두 값 모두 `expected`와 다름 | 두 load가 서로 다른 잘못된 값을 반환했습니다. | CPU 이동, 동시 상태 변화와 transient data path를 확인합니다. |
| CRC mismatch 후 slow compare 정상 | Checksum 계산과 slow compare에서 관찰한 데이터가 달랐습니다. | 두 검사 사이의 시간, CPU와 memory 상태를 확인합니다. |

`reread == expected`이면 일반 RAM 로그에 `read error`가 추가됩니다. 이 문자열은 첫 read와 reread 결과를 이용한 software 분류입니다. Hardware 발생 위치를 판정하려면 CPU, cache, controller와 RAS 정보를 함께 확인합니다.

`read != expected`가 확인된 상세 로그는 CPU가 관찰한 memory data miscompare입니다. Stressapptest는 이를 `Hardware Error`로 집계합니다. Upstream도 stressapptest의 범위를 memory cell, cache coherency, memory controller와 bus interface를 포함하는 memory-interface 검사로 설명합니다.

## `Report Error`와 최종 결과

초기 상세 오류는 parse 가능한 `Report Error`도 생성할 수 있습니다.

```text
Report Error: miscompare : <part> : <count> : <time-to-failure>
```

`time-to-failure`는 `OsLayer` 초기화 이후 경과 시간입니다. 상세 mismatch의 wall-clock timestamp와 기준이 다르므로 분석할 때 두 시간 기준을 구분합니다.

시험 종료 시에는 다음 순서로 결과를 확인합니다.

```text
Log: Thread <id> found <count> hardware incidents
Stats: Found <total> hardware incidents
Status: FAIL - test discovered HW problems
```

상세 로그 수는 전체 incident 수보다 적을 수 있습니다. 최종 `Stats` 값을 전체 집계로 사용합니다.

## DRAM 주파수 기록

이 fork의 DDR 옵션을 사용하면 control node에 설정값을 전달하고 성공한 값을 내부 상태에 저장합니다.

```bash
# 한 값으로 실행
stressapptest -M 1024 -m 4 -i 4 -s 600 \
  --ddr-freq 3196 -l /data/local/tmp/stressapptest.log

# 지원 목록을 기본 3초 간격으로 순환
stressapptest -M 1024 -m 4 -i 4 -s 600 \
  --ddr-freq all -l /data/local/tmp/stressapptest.log

# 전환 간격 지정
stressapptest -M 1024 -m 4 -i 4 -s 600 \
  --ddr-freq all --ddr-step 5
```

`--ddr-freq`를 생략하면 control node에 접근하지 않습니다. `--ddr-step`의 기본값은 3초입니다.

설정값 전달에 성공하면 다음 로그가 생성됩니다.

```text
Log: DDR_FREQ write=<value> monotonic_us=<time> node=<control_node>
```

이 로그의 `write`는 control node의 `write()`가 성공한 값을 뜻합니다. 실제 hardware clock 확인에는 target이 제공하는 readback 또는 hardware counter를 사용합니다.

### 오류 record에 저장되는 주파수

| 필드 | 저장 시점 |
|---|---|
| `write` | Fill, copy, invert, file read 또는 network receive로 block을 마지막 전체 기록한 시점 |
| `read` | `CheckRegion()`에서 mismatch 값을 읽기 직전 |
| `reread` | `ProcessError()`에서 같은 주소를 다시 읽기 직전 |

Block 기록과 검사 사이에 주파수가 전환되면 세 값이 달라질 수 있습니다. `CheckRegion()`이 여러 오류를 먼저 저장한 뒤 처리하므로 `read`와 `reread` 사이에도 전환이 발생할 수 있습니다.

세 값은 각 동작 시점에 `ErrorRecord`에 저장됩니다. Logger queue의 출력 지연은 저장된 값에 영향을 주지 않습니다.

### 시간 순서 확인

주파수 전환 시험에서는 다음 시각을 구분합니다.

```text
Control node write 시작
 → write() 반환
 → hardware clock·voltage·timing 전환
 → Worker의 write 또는 read
 → mismatch 검출
 → ProcessError()의 reread
 → Logger queue 저장
 → logfile과 stdout 출력
```

Stressapptest 기본 timestamp는 `ProcessError()`가 메시지를 만든 시각에 가깝습니다. 데이터 상태가 처음 변경된 시각은 직접 기록하지 않습니다. 주파수 경계 분석에는 monotonic timestamp와 actual-frequency readback을 함께 사용합니다.

## 고정 주파수와 전환 시험 분리

주파수 연관성을 확인할 때는 다음 순서로 실행합니다.

1. 각 주파수를 고정하여 같은 명령을 반복합니다.
2. 주파수 순환 시험을 별도로 실행합니다.
3. 전환 직전, 전환 처리 구간과 안정 구간을 구분합니다.
4. 설정값과 hardware readback 값을 같은 monotonic 시간축에 기록합니다.
5. 같은 실행의 temperature, voltage, memory-controller와 RAS 정보를 함께 저장합니다.

고정 시험과 전환 시험을 분리하면 안정 상태와 DDR DVFS 처리 구간을 각각 비교할 수 있습니다.

## 로그를 수집할 때 확인할 항목

첫 상세 mismatch를 기준으로 다음 정보를 저장합니다.

```text
Binary build ID와 source commit
전체 실행 명령
Worker 종류와 수
CPU affinity
Virtual address와 확인 가능한 physical address
read, reread, expected
read XOR expected
마지막 writer CPU
DDR 설정값과 actual-frequency readback
Monotonic timestamp
Temperature와 voltage
Memory-controller, cache, RAS와 kernel error log
최종 hardware incident 수와 종료 원인
```

오류 행이 연속으로 출력되면 주소 범위와 XOR bit를 기준으로 묶습니다. 하나의 넓은 데이터 변경이 여러 64-bit mismatch 행으로 기록될 수 있습니다.

## 분석 결과 작성 형식

로그만으로 확인된 내용과 추가 확인이 필요한 범위를 구분하여 기록합니다.

```text
확인 결과:
- Worker의 checksum 검사에서 mismatch가 발생함
- Slow compare의 read 값이 expected와 다름
- Reread 결과와 DDR 설정 metadata를 기록함

추가 확인:
- 최초 mismatch load를 실행한 CPU
- Reread가 완료된 cache 또는 memory 계층
- Actual DDR clock과 전환 완료 시각
- Memory-controller·PHY·DRAM의 hardware error 정보
```

이 형식을 사용하면 stressapptest가 직접 확인한 사실과 hardware 계층별 원인 분석을 분리해서 관리할 수 있습니다.

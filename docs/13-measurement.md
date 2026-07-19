# 부하와 오류를 측정하는 방법

Stressapptest의 출력은 프로그램이 논리적으로 처리한 데이터양을 보여줍니다. 실제 LPDDR 읽기·쓰기 데이터양과 명령 수를 확인하려면 CPU cache, SLC, NoC, DMC의 측정값을 함께 확인해야 합니다.

## 어디에서 무엇을 측정하는가

```text
stressapptest의 처리량 계산값
        ↓
CPU PMU: 읽기·쓰기 명령, L1·L2 refill과 write-back
        ↓
SLC/LLCC PMU
        ↓
NoC/interconnect PMU
        ↓
DMC PMU: 읽기·쓰기 byte 또는 명령 수
        ↓
LPDDR PHY/controller telemetry
```

한 계층의 counter만으로 다른 계층의 데이터 이동량을 정확히 계산할 수는 없습니다.

## Stressapptest가 출력하는 값

Stressapptest는 Worker가 처리한 block 수와 실행 시간을 사용하여 다음 값을 출력합니다.

- 전체 논리적 처리량 MB와 MB/s
- Memory Copy MB/s
- Data Check MB/s
- Invert Data MB/s
- File/Net/Disk MB/s
- Worker별 오류 수
- Queue에서 각 block이 선택된 횟수의 분포

> **파일:** `src/sat.cc` · **함수:** `Sat::AnalysisAllStats()` · **기준:** `73b9df2`

```cpp
for (WorkerMap::const_iterator map_it = workers_map_.begin();
     map_it != workers_map_.end(); ++map_it) {
  for (WorkerVector::const_iterator it = map_it->second->begin();
       it != map_it->second->end(); ++it) {
    thread_runtime_sec = (*it)->GetRunDurationUSec() / 1000000.0;
    total_data += (*it)->GetMemoryCopiedData();
    total_data += (*it)->GetDeviceCopiedData();
    if (thread_runtime_sec > max_runtime_sec)
      max_runtime_sec = thread_runtime_sec;
  }
}
total_bandwidth = total_data / max_runtime_sec;
```

**코드 설명:** 전체 bandwidth는 각 Worker가 보고한 논리적 메모리·장치 처리량을 더한 뒤 가장 오래 실행된 Worker의 실행 시간으로 나눕니다. DMC counter에서 측정한 읽기·쓰기 byte를 사용하지 않습니다. 따라서 stressapptest의 MB/s와 LPDDR의 실제 MB/s는 서로 다를 수 있습니다.

`CopyThread`는 원본 읽기와 대상 쓰기를 더하여 block 크기의 두 배를 논리적 처리량으로 계산합니다. 실제 DMC 처리량과 차이가 나는 원인은 다음과 같습니다.

- 대상 쓰기의 write allocate와 쓰기 권한 요청
- 원본 또는 대상 cache hit
- Prefetch했지만 사용하지 않은 cache line
- Dirty cache line의 write-back 지연
- Checksum 불일치 이후 재검사
- SLC hit와 cache 사이의 직접 데이터 전달
- bionic `memcpy()`의 연속 복사 최적화
- Queue, 로그, kernel이 발생시키는 메모리 접근

## CPU PMU

확인할 수 있는 event 종류는 다음과 같습니다.

- 완료된 명령 수와 CPU cycle
- L1D 접근과 refill
- L1D writeback
- L2 access/refill/writeback
- last-level cache access/miss
- Bus 접근
- 메모리 응답을 기다린 시간

Android `simpleperf`에서 실제 사용할 수 있는 event를 먼저 확인합니다.

```bash
adb shell simpleperf list
```

Event 이름과 접근 권한은 kernel과 SoC에 따라 다릅니다. `cache-misses`는 특정 cache 계층에서 데이터를 찾지 못한 횟수입니다. 실제 DRAM 접근 여부는 SLC, NoC, DMC counter를 추가로 확인해야 합니다.

## SLC/LLCC PMU

모바일 SoC에는 CPU cluster cache 밖에 system cache가 있을 수 있습니다. 다음 항목을 확인합니다.

- SLC lookup/hit/miss
- read allocate
- dirty eviction/writeback
- 요청 장치별 측정이 가능하면 CPU·UFS·GPU 분리
- slice/bank별 occupancy 또는 bandwidth

L2 miss가 발생해도 SLC에서 데이터를 찾으면 LPDDR 읽기는 발생하지 않을 수 있습니다.

## NoC PMU

NoC PMU를 사용할 수 있으면 다음 항목을 확인합니다.

- CPU가 발생시킨 읽기·쓰기 flit
- DMC port의 데이터 이동량
- 완료를 기다리는 요청 수
- QoS stall
- read/write latency
- channel/port별 utilization

NoC counter는 protocol header, snoop, 재시도 요청을 포함할 수도 있고 실제 데이터 byte만 셀 수도 있습니다. SoC 문서에서 각 counter의 정의를 확인해야 합니다.

## DMC PMU

DMC는 LPDDR 명령을 직접 배치하므로 다음 counter를 LPDDR 부하 분석에 사용합니다.

- 읽은 데이터 byte
- 쓴 데이터 byte
- 읽기·쓰기 명령 수
- active/precharge command
- Row hit·miss 또는 page hit
- bank conflict
- queue occupancy
- channel별 utilization
- DMC clock/devfreq state

명령 수만 제공하는 counter에서 데이터양을 계산하려면 burst length와 data width뿐 아니라 rank·channel, 부분 쓰기, 압축, DBI, ECC가 counter에 어떻게 반영되는지 확인해야 합니다.

## 초기 쓰기·Worker 실행·마지막 검사 구분

`-s`는 본 시험 시간만 나타내므로 초기 데이터 쓰기와 마지막 전체 검사 시간을 포함하지 않습니다. Stressapptest 로그와 PMU 측정 시각을 맞춰 다음 작동 단계를 구분합니다.

```text
T0 프로그램 시작
T1 mmap 완료
T2 초기 데이터 쓰기 시작
T3 초기 데이터 쓰기 종료와 queue 상태 설정
T4 본 시험 Worker 시작
T5 Worker 일시 정지
T6 Worker 재시작
T7 본 시험 Worker 종료
T8 마지막 전체 검사 시작과 종료
T9 프로그램 종료
```

로그 상세도를 12 이상으로 설정하면 초기 데이터 쓰기와 Worker 실행 로그가 더 많이 출력됩니다. 로그 I/O로 인한 추가 부하도 증가합니다.

## Worker별로 예상되는 측정 결과

| 실행 방식 | CPU 동작 | Cache 동작 | 예상되는 DMC 경향 |
|---|---|---|---|
| `-m 0 -c N` | Checksum 계산 | Refill 중심 | 읽기 비율 증가 |
| `-m N` | Checksum과 복사 | Refill과 dirty write-back | 읽기와 쓰기 모두 증가 |
| `-m N -W` | NEON·FP-SIMD | Prefetch와 dirty write-back | 읽기·쓰기 혼합, CPU 구현에 따라 차이 |
| `-m N -F` | 최적화된 `memcpy()` | 연속 접근 최적화 가능 | 높은 읽기·쓰기 bandwidth 가능 |
| `-m 0 -i N` | RMW와 barrier | Cache 관리 명령이 많음 | 읽기·쓰기 발생, 효율이 낮을 수 있음 |
| `--cc_test` | CPU 사이 쓰기 권한 이동 | Snoop과 ownership 요청 | DRAM bandwidth는 낮을 수 있음 |
| `-C N` | 부동소수점 연산 | L1에 데이터가 남을 수 있음 | DRAM에 미치는 직접 영향이 작음 |

## Physical address와 channel별 접근량 확인

`/proc/self/pagemap`과 `--memory_channel`은 주소를 분석하기 위한 프로그램의 추정값을 제공합니다. 실제 channel별 접근량은 channel별 DMC counter로 측정해야 합니다.

<sub><em>PMU: Performance Monitoring Unit의 약어이며 hardware event 발생 횟수를 집계하는 장치입니다.</em></sub><br>
<sub><em>DMC traffic: DRAM memory controller가 channel별로 처리한 read/write byte 또는 command입니다.</em></sub>

Channel별 DMC counter를 사용할 수 있으면 같은 명령을 여러 번 실행하여 다음 항목을 확인합니다.

- channel 0/1 read bytes 균형
- Worker 수 증가에 따른 처리량 변화
- Physical page 할당 상태에 따른 실행별 차이

Physical address에서 channel을 선택하는 규칙을 모르면 SAT block 주소만으로 channel 불균형의 원인을 확정하기 어렵습니다.

## 최소 기록 항목

```text
실행 파일의 commit/build-id:
NDK/AOSP branch/compiler flags:
실행 명령:
Android build/kernel:
SoC/DRAM density/speed bin:
CPU online/cpuset/affinity:
governor and starting frequency:
DMC governor/frequency:
시작 온도와 전원 방식:
각 작동 단계의 timestamp:
stressapptest 논리적 MB/s와 오류 수:
CPU PMU:
SLC/NoC PMU:
DMC read/write bytes/commands:
kernel/LMKD/thermal/RAS logs:
종료 원인:
```

## 실행 중 프로그램이 종료된 경우

Stressapptest 로그가 중간에 끊겼다는 이유만으로 메모리 데이터 불일치라고 판단할 수 없습니다. 다음 종료 원인을 구분해야 합니다.

- Stressapptest가 오류를 기록하고 종료 코드 1로 종료
- 메모리 할당 실패 또는 assertion 발생
- LMKD/OOM SIGKILL
- thermal shutdown
- watchdog reset
- kernel panic/SError
- secure firmware reset
- 사용자 종료 또는 자동화 도구의 시간 제한

`pstore`, reboot reason, kernel 로그, LMKD, 온도, DMC·RAS의 timestamp를 stressapptest의 작동 단계와 함께 확인해야 합니다.

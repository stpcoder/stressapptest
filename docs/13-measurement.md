# 부하와 오류를 측정하는 방법

stressapptest 출력은 프로그램이 처리한 양을 보여줍니다. 실제 LPDDR read/write 양과 command 수를 확인하려면 CPU cache, SLC, NoC, DMC 계측값을 함께 봐야 합니다.

## 어디에서 무엇을 측정하는가

```text
SAT software counters
        ↓
CPU PMU: load/store, L1/L2 refill/writeback
        ↓
SLC/LLCC PMU
        ↓
NoC/interconnect PMU
        ↓
DMC PMU: read/write bytes 또는 commands
        ↓
LPDDR PHY/controller telemetry
```

한 계층의 counter만으로 다른 계층의 traffic을 정확히 계산할 수는 없습니다.

## stressapptest 출력값

SAT는 worker의 처리 block 수와 runtime으로 다음을 출력한다.

- total logical MB와 MB/s
- Memory Copy MB/s
- Data Check MB/s
- Invert Data MB/s
- File/Net/Disk MB/s
- worker error count
- queue touch histogram

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

**해석:** SAT 전체 bandwidth는 worker가 보고한 논리적 memory/device data의 합을 가장 긴 worker runtime으로 나눈 값입니다. DMC counter에서 관측한 read/write byte의 합을 직접 사용하지 않습니다. 따라서 SAT MB/s와 LPDDR MB/s는 동일한 수치가 될 필요가 없습니다.

CopyThread의 논리값은 block마다 source read와 destination write를 합쳐 2배로 계산한다. 실제 DMC traffic과 차이가 날 수 있는 원인:

- destination write allocate/RFO
- source/destination cache hit
- prefetch가 가져온 unused line
- dirty writeback 시점 지연
- checksum mismatch reread
- SLC hit 및 cache-to-cache transfer
- bionic memcpy streaming 최적화
- queue/log/kernel memory traffic

## CPU PMU

가능한 event category:

- retired instructions/cycles
- L1D access/refill
- L1D writeback
- L2 access/refill/writeback
- last-level cache access/miss
- bus access
- stall/backend memory latency

Android `simpleperf`에서 실제 사용 가능한 event를 먼저 조회한다.

```bash
adb shell simpleperf list
```

event 이름과 권한은 kernel/SoC마다 다르다. `cache-misses`는 해당 cache level의 miss 횟수를 나타내며, DRAM access는 SLC/NoC/DMC counter를 추가하여 판정한다.

## SLC/LLCC PMU

mobile SoC에는 CPU cluster cache 밖에 system cache가 있을 수 있다. 확인할 category:

- SLC lookup/hit/miss
- read allocate
- dirty eviction/writeback
- master별 traffic 가능 시 CPU/UFS/GPU 분리
- slice/bank별 occupancy 또는 bandwidth

L2 miss가 SLC hit이면 LPDDR read가 발생하지 않을 수 있다.

## NoC PMU

관찰 가능한 경우:

- CPU master read/write flit
- DMC port traffic
- outstanding transaction
- QoS stall
- read/write latency
- channel/port별 utilization

NoC byte는 protocol header, snoop, retry가 포함되거나 data byte만 셀 수 있어 counter 정의 확인이 필요하다.

## DMC PMU

DMC는 LPDDR command를 직접 schedule하므로 다음 counter를 LPDDR 부하 분석에 사용한다.

- read data bytes
- write data bytes
- read/write command count
- active/precharge command
- row hit/miss 또는 page hit
- bank conflict
- queue occupancy
- channel별 utilization
- DMC clock/devfreq state

counter가 command 단위라면 burst length와 data width를 적용할 때 rank/channel, partial write, compression/DBI/ECC 정의를 확인한다.

## 초기 fill·실행·마지막 검사를 나눠서 측정하기

`-s` timer만 보면 초기 fill과 final check가 섞인다. SAT log timestamp와 PMU sample을 맞춰 다음 phase를 구분한다.

```text
T0 process start
T1 mmap 완료
T2 fill start
T3 fill end / queue tagging
T4 timed worker start
T5 pause
T6 resume
T7 timed worker stop
T8 final check start/end
T9 process exit
```

verbosity 12 이상이면 fill/worker phase log를 더 많이 볼 수 있지만 log I/O overhead도 증가한다.

## Worker별로 예상되는 계측 특징

| Mode | CPU | Cache | DMC 기대 경향 |
|---|---|---|---|
| `-m 0 -c N` | checksum ALU | refill 중심 | read 비중 증가 |
| `-m N` | checksum+copy | refill+dirty WB | read/write mixed |
| `-m N -W` | NEON/FP-SIMD | prefetch+dirty WB | mixed, 구현 의존 |
| `-m N -F` | memcpy 최적화 | streaming 가능 | 높은 mixed BW 가능 |
| `-m 0 -i N` | RMW+barrier | maintenance 많음 | read/write, 효율 낮을 수 있음 |
| `--cc_test` | core ping-pong | snoop/ownership | DRAM BW 낮을 수 있음 |
| `-C N` | FP compute | L1 resident 가능 | DRAM 직접 영향 작음 |

## Physical address와 channel 확인

SAT의 `/proc/self/pagemap`과 `--memory_channel`은 software address 진단값을 제공한다. 실제 channel traffic은 channel별 DMC counter로 측정한다.

<sub><em>PMU: Performance Monitoring Unit의 약어이며 hardware event 발생 횟수를 집계하는 장치입니다.</em></sub><br>
<sub><em>DMC traffic: DRAM memory controller가 channel별로 처리한 read/write byte 또는 command입니다.</em></sub>

실제 channel별 DMC counter가 있다면 동일 command를 여러 번 실행해:

- channel 0/1 read bytes 균형
- worker 수 증가에 따른 scaling
- physical allocator 상태에 따른 run-to-run 변화

를 관찰한다. PA→channel hash가 알려지지 않은 상태에서는 channel 불균형 원인을 SAT block address만으로 확정하기 어렵다.

## 최소 기록 항목

```text
binary commit/build-id:
NDK/AOSP branch/compiler flags:
command line:
Android build/kernel:
SoC/DRAM density/speed bin:
CPU online/cpuset/affinity:
governor and starting frequency:
DMC governor/frequency:
starting temperature/power mode:
SAT phase timestamps:
SAT logical MB/s/errors:
CPU PMU:
SLC/NoC PMU:
DMC read/write bytes/commands:
kernel/LMKD/thermal/RAS logs:
exit reason:
```

## 실행 중 process가 사라진 경우

SAT log가 끊겼다는 사실만으로 memory miscompare라고 판단할 수 없다. 다음을 구분한다.

- SAT가 error를 기록하고 exit 1
- allocation failure 또는 assertion
- LMKD/OOM SIGKILL
- thermal shutdown
- watchdog reset
- kernel panic/SError
- secure firmware reset
- user/automation timeout

`pstore`, reboot reason, kernel log, LMKD, thermal 및 DMC/RAS timestamp를 SAT phase와 함께 확인한다.

# PMU·traffic·결과 측정

## 측정 계층

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

상위 계층 counter 하나만으로 아래 계층을 완전히 추론할 수 없다.

## SAT 자체 통계

SAT는 worker의 처리 block 수와 runtime으로 다음을 출력한다.

- total logical MB와 MB/s
- Memory Copy MB/s
- Data Check MB/s
- Invert Data MB/s
- File/Net/Disk MB/s
- worker error count
- queue touch histogram

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

event 이름과 권한은 kernel/SoC마다 다르다. `cache-misses` 하나를 DRAM access로 간주하지 않는다.

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

LPDDR 부하 확인에 가장 직접적인 항목이다.

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

## 단계별 traffic을 분리하는 방법

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

## Worker별 기대 signature

| Mode | CPU | Cache | DMC 기대 경향 |
|---|---|---|---|
| `-m 0 -c N` | checksum ALU | refill 중심 | read 비중 증가 |
| `-m N` | checksum+copy | refill+dirty WB | read/write mixed |
| `-m N -W` | NEON/FP-SIMD | prefetch+dirty WB | mixed, 구현 의존 |
| `-m N -F` | memcpy 최적화 | streaming 가능 | 높은 mixed BW 가능 |
| `-m 0 -i N` | RMW+barrier | maintenance 많음 | read/write, 효율 낮을 수 있음 |
| `--cc_test` | core ping-pong | snoop/ownership | DRAM BW 낮을 수 있음 |
| `-C N` | FP compute | L1 resident 가능 | DRAM 직접 영향 작음 |

## Physical address와 channel 측정

SAT의 `/proc/self/pagemap` 및 `--memory_channel` 추정을 실제 channel traffic과 혼동하지 않는다.

실제 channel별 DMC counter가 있다면 동일 command를 여러 번 실행해:

- channel 0/1 read bytes 균형
- worker 수 증가에 따른 scaling
- physical allocator 상태에 따른 run-to-run 변화

를 관찰한다. PA→channel hash가 알려지지 않은 상태에서는 channel 불균형 원인을 SAT block address만으로 확정하기 어렵다.

## 최소 기록 template

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

## Process가 사라진 경우

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

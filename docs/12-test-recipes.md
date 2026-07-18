# 목적별 테스트 명령

이 장에서는 read/check 중심, 기본 copy, RMW, CPU 결합 부하처럼 확인할 목적에 따라 실행 명령을 나눠 설명합니다. 한 번에 한 조건만 바꾸면 traffic 변화의 원인을 비교하기 쉽습니다.

## 테스트 조건을 바꾸는 순서

한 번에 option을 여러 개 바꾸면 traffic 원인을 분리하기 어렵다. 다음 순서로 한 축씩 바꾼다.

1. memory 크기 고정
2. CPU affinity/cpuset 고정
3. worker 종류 하나 선택
4. worker 수 sweep
5. `-W` 또는 `-F`만 추가 비교
6. 필요하면 CPU/UFS/coherency 부하 결합

각 run은 시작 온도, governor, background workload, screen 상태, charger/전원 조건을 같게 맞춘다.

## 각 명령이 사용하는 코드 경로

> **파일:** `src/worker.cc` · **함수:** `CopyThread::Work()` · **기준:** `73b9df2`

```cpp
if (sat_->warm()) {
  CrcWarmCopyPage(&dst, &src);       // -W
} else if (sat_->strict()) {
  CrcCopyPage(&dst, &src);           // 기본
} else {
  memcpy(dst.addr, src.addr,
         sat_->page_length());       // -F
}
```

**해석:** recipe에서 `-W`는 ARM64 vector checksum copy, 기본 실행은 C checksum copy, `-F`는 libc `memcpy`를 선택합니다. 조건 순서상 `-W -F`를 함께 지정하면 `-W` 경로가 실행됩니다. 이 구분을 유지해야 CPU instruction mix와 DMC traffic 차이를 해석할 수 있습니다.

아래 명령의 `512` MiB와 worker 수는 예시다. target RAM과 thermal 여유에 맞게 조정한다.

## 0. 초기 fill 관찰

```bash
stressapptest -M 512 -s 1 -m 0 -c 0 -v 12
```

예상 phase:

1. 8개 FillThread가 512 MiB 전체 write
2. timed worker 구간은 worker metadata 처리만 수행
3. final 8개 CheckThread가 valid block read

짧은 `-s`에도 fill/final check가 있으므로 write-heavy init와 read-heavy final을 분리해 관찰할 수 있다. 단, phase 지속 시간은 빠를 수 있으므로 trace timestamp가 필요하다.

## 1. Read/check 중심

```bash
stressapptest -M 512 -s 60 -m 0 -c 4
```

예상:

- 4개의 CheckThread가 random valid 1 MiB block 선택
- block 내부 4 KiB checksum, 순차 load
- test data destination write 없음
- queue metadata write는 존재
- DMC read 비중이 높아질 가능성

worker sweep:

```text
-c 1 → 2 → 4 → 8
```

core 수 증가에 따른 read bandwidth scaling, cache refill, DMC queue saturation을 본다.

## 2. 기본 mixed read/write

```bash
stressapptest -M 512 -s 60 -m 4
```

예상:

- source 1 MiB sequential read
- checksum 계산
- destination 1 MiB sequential cached write
- destination dirty eviction/writeback
- source/destination block은 loop마다 random

SAT의 대표 correctness-under-load mode다.

## 3. High-throughput memcpy 비교

```bash
stressapptest -M 512 -s 60 -m 4 -F
```

기본 mode와 비교할 항목:

- SAT reported MB/s
- CPU cycles/instructions
- L1/L2/SLC refill
- DMC read/write bytes
- destination write 검출 지연

`-F`에서 DMC bandwidth가 증가한 경우에는 checksum arithmetic 제거 효과와 bionic `memcpy()` 구현의 load/store 효율을 분석한다. cache bypass 여부는 instruction trace, cache PMU 및 DMC counter를 사용하여 별도로 판정한다.

<sub><em>Bottleneck: 전체 처리율을 제한하는 계산, memory 또는 synchronization 자원입니다.</em></sub>

## 4. ARM64 warm/vector copy

```bash
stressapptest -M 512 -s 60 -m 4 -W
```

예상:

- 64 B `ld1/st1`
- `prfm pldl1strm`
- vector checksum add
- 일반 cached destination

비교 대상:

```text
default vs -W vs -F
```

세 mode의 CPU utilization, retired instruction 및 DMC bandwidth를 함께 측정하여 checksum, vector copy 및 libc `memcpy()` 구현의 영향을 분리한다.

## 5. RMW/invert 중심

```bash
stressapptest -M 512 -s 60 -m 0 -i 4
```

예상:

- 같은 block을 up/down 방향으로 네 번 RMW
- 시작과 끝 checksum read
- 64 B마다 ARM64 `dc cvau` 기반 maintenance
- 많은 barrier와 cache state transition

Invert workload의 결과는 four-pass RMW, 방향 전환, cache maintenance 및 barrier 비용을 포함한다. Copy workload와의 비교에는 각 mode의 logical byte 계산과 DMC read/write byte를 함께 사용한다.

## 6. CPU power 결합

```bash
stressapptest -M 512 -s 300 -m 4 -C 4
```

예상:

- CopyThread의 memory traffic
- CpuStressThread의 FP calculation
- shared power/thermal budget 경쟁
- DVFS/thermal throttling 가능성

이 구성은 SoC total power 및 thermal 조건을 증가시키는 실험에 사용한다. `-C` 증가로 CPU power budget 사용량이 증가하면 DVFS 또는 thermal 제한에 의해 memory worker throughput이 감소할 수 있다.

<sub><em>Power corner: 여러 hardware block의 동시 동작으로 전력, 전압 또는 온도 조건이 한계에 접근하는 시험 상태입니다.</em></sub>

## 7. Cache coherency 집중

```bash
stressapptest -M 256 -s 60 -m 0 --cc_test
```

예상:

- CPU 수만큼 coherency thread
- 소수 shared cache line counter ping-pong
- snoop/ownership traffic 증가
- DRAM data bandwidth는 낮을 수 있음

다음 option으로 공유 line 수와 batch 크기를 바꾼다.

```bash
stressapptest -M 256 -s 60 -m 0 \
  --cc_test --cc_line_count 8 --cc_inc_count 10000
```

## 8. Wrong-address/tag 검출

```bash
stressapptest -M 512 -s 60 -m 4 --tag_mode
```

각 cache line 첫 word가 virtual address tag가 된다. 일반 pattern test와 결과가 달라질 수 있으므로 별도 campaign으로 관리한다. File/Network/Disk option과 결합하지 않는다.

## 9. Pause/resume power step

```bash
stressapptest -M 512 -s 300 -m 8 \
  --pause_delay 60 --pause_duration 10
```

약 60초마다 power-spike 그룹 worker를 10초 멈췄다가 재개한다. resume 순간의:

- CPU frequency ramp
- DMC frequency transition
- PMIC current step
- thermal response
- memory error timestamp

를 함께 관찰한다.

## 10. File/UFS DMA 결합

filesystem에 충분한 여유가 있고 수명 영향이 허용될 때만 사용한다.

```bash
stressapptest -M 512 -s 120 -m 4 \
  -f /data/local/tmp/sat-a.bin
```

이 mode는 CPU memory copy와 UFS/filesystem/DMA traffic을 동시에 만든다. 순수 LPDDR 분석과 storage-path 분석 결과를 구분한다.

## 비교할 테스트 조합

| Case | 명령 핵심 | 주된 관찰 |
|---|---|---|
| A | `-m 0 -c N` | read/check scaling |
| B | `-m N` | checksum mixed R/W |
| C | `-m N -W` | ARM64 vector copy |
| D | `-m N -F` | optimized memcpy throughput |
| E | `-m 0 -i N` | RMW + maintenance |
| F | `-m N -C N` | memory+CPU power corner |
| G | `-m 0 --cc_test` | cache coherency ping-pong |
| H | `-m N -f file` | memory+storage DMA |

각 case에서 최소 3회 반복하고 cold/hot 결과를 분리한다.

## Write-only traffic이 필요한 경우

현재 public option에는 지속적인 pure write-only memory worker가 없다.

- FillThread: 초기화 구간에서 한 번 실행되는 write-heavy worker
- CopyThread: source read + destination write
- InvertThread: read-modify-write
- CheckThread: read-only

순수 write stream이 필요하면 별도 worker를 구현하거나 전문 bandwidth microbenchmark를 병행해야 한다. stressapptest의 목적은 transaction correctness이므로 이 한계는 설계 의도와 관련된다.

# LPDDR 분석용 시험 recipe

## 실험 설계 원칙

한 번에 option을 여러 개 바꾸면 traffic 원인을 분리하기 어렵다. 다음 순서로 한 축씩 바꾼다.

1. memory 크기 고정
2. CPU affinity/cpuset 고정
3. worker 종류 하나 선택
4. worker 수 sweep
5. `-W` 또는 `-F`만 추가 비교
6. 필요하면 CPU/UFS/coherency 부하 결합

각 run은 시작 온도, governor, background workload, screen 상태, charger/전원 조건을 같게 맞춘다.

아래 명령의 `512` MiB와 worker 수는 예시다. target RAM과 thermal 여유에 맞게 조정한다.

## 0. 초기 fill 관찰

```bash
stressapptest -M 512 -s 1 -m 0 -c 0 -v 12
```

예상 phase:

1. 8개 FillThread가 512 MiB 전체 write
2. timed worker는 data traffic을 거의 만들지 않음
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

`-F`에서 DMC bandwidth가 증가하면 checksum arithmetic이 bottleneck이었거나 bionic memcpy가 더 효율적인 store path를 사용했을 가능성이 있다. 이것은 직접 cache bypass의 증거는 아니다.

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

세 mode의 CPU utilization과 DMC bandwidth를 함께 보면 checksum/vector/memcpy implementation 차이를 분리하기 쉽다.

## 5. RMW/invert 중심

```bash
stressapptest -M 512 -s 60 -m 0 -i 4
```

예상:

- 같은 block을 up/down 방향으로 네 번 RMW
- 시작과 끝 checksum read
- 64 B마다 ARM64 `dc cvau` 기반 maintenance
- 많은 barrier와 cache state transition

Copy workload와 성격이 매우 다르므로 “write bandwidth mode”로 단순 비교하지 않는다.

## 6. CPU power 결합

```bash
stressapptest -M 512 -s 300 -m 4 -C 4
```

예상:

- CopyThread의 memory traffic
- CpuStressThread의 FP calculation
- shared power/thermal budget 경쟁
- DVFS/thermal throttling 가능성

LPDDR 자체가 아니라 SoC total power corner를 만들고 싶을 때 사용한다. `-C`만 늘리면 memory bandwidth가 오히려 감소할 수도 있다.

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

## 권장 실험 matrix

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

## 순수 write-only가 필요한 경우

현재 public option에는 지속적인 pure write-only memory worker가 없다.

- FillThread: 초기 write-heavy, timed loop 아님
- CopyThread: source read + destination write
- InvertThread: read-modify-write
- CheckThread: read-only

순수 write stream이 필요하면 별도 worker를 구현하거나 전문 bandwidth microbenchmark를 병행해야 한다. stressapptest의 목적은 transaction correctness이므로 이 한계는 설계 의도와 관련된다.

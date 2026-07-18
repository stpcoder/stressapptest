# SAT block, page_entry, valid/empty queue

## SAT block은 무엇인가?

stressapptest는 전체 allocation을 `page_length_` 단위로 나눈다. 기본값은 1 MiB이며 `-p`로 변경할 수 있다.

```text
pages_ = test memory bytes / SAT block bytes
```

소스에서 이를 page라고 부르지만 Linux MMU page와 다르다.

| 개념 | 기본 크기 | 누가 관리하는가 |
|---|---:|---|
| SAT block | 1 MiB | stressapptest queue |
| checksum slice | 4 KiB | CrcCopy/CrcCheck loop |
| Linux page | 기기 설정 의존 | kernel/MMU |
| cache line | 통상 64 B | CPU/cache coherency |
| LPDDR row | device density/map 의존 | DMC/DRAM |

## `page_entry` metadata

각 SAT block은 다음 metadata로 표현된다 (`src/queue.h:38`).

| field | 의미 |
|---|---|
| `offset` | 전체 allocation base에서의 virtual offset |
| `addr` | block을 사용할 때 준비된 현재 virtual pointer |
| `paddr` | block 첫 주소의 진단용 possible PA |
| `pattern` | 이 block에 있어야 하는 Pattern pointer. null이면 empty |
| `tag` | generic region/NUMA 선택 mask |
| `touch` | valid block 선택 횟수 또는 queue metric |
| `ts` | 마지막 GetValid 시 timestamp |
| `lastcpu` | 마지막 write를 수행한 CPU |
| `lastpattern` | 마지막 read 시점의 pattern |

`paddr`는 block 전체가 physical contiguous임을 의미하지 않는다. 첫 주소의 PA만 저장한다.

## Empty와 Valid의 뜻

### Valid

- `pattern != NULL`
- memory 내용이 해당 pattern이라고 기대됨
- source/check/file/network output으로 사용 가능

### Empty

- `pattern == NULL`
- destination으로 덮어써도 됨
- OS에서 free된 memory가 아님
- mmap과 physical backing이 해제된 상태도 아님

상태 전이는 다음과 같다.

```text
Empty
  │ Fill 또는 Copy destination write
  ▼
Valid(pattern=P)
  │ Copy source로 소비
  ▼
Empty
```

CheckThread는 timed run 중에는 검사한 block을 다시 valid로 넣고, final check에서는 empty로 바꿔 queue를 소진한다.

## 기본 FineLockPEQueue

기본 구현은 실제 FIFO queue가 아니라 `page_entry[]` array와 block별 mutex다 (`src/finelock_queue.cc`).

```text
page_entry[0] + lock[0]
page_entry[1] + lock[1]
...
page_entry[N] + lock[N]
```

Get operation은:

1. random 시작 index 선택
2. linear-congruential progression으로 array 탐색
3. valid/empty predicate와 tag 확인
4. 해당 block mutex를 `trylock`
5. 성공한 entry를 worker에 반환

Put operation은 metadata를 같은 array slot에 기록하고 mutex를 unlock한다.

장점:

- 하나의 global queue lock contention 감소
- 서로 다른 worker가 다른 block을 동시에 획득 가능
- 시작점과 탐색 순서를 섞어 특정 block 편향 감소

## 왜 random이지만 block 내부는 sequential인가?

주소 선택에는 두 granularity가 있다.

```text
큰 단위: 어느 1 MiB block인가? → pseudo-random
작은 단위: block 안에서 어느 byte인가? → 앞에서 뒤로 순차
```

이 조합은 여러 멀리 떨어진 memory stream을 만들면서 각 stream 안에서는 hardware prefetch와 burst 효율을 유지한다.

완전 random 64 B access와는 다른 workload다. 완전 random access보다 bandwidth가 높고, 단일 sequential buffer보다 cache reuse가 낮아지는 방향이다.

## Valid/Empty 비율

초기 fill은 모든 block을 먼저 valid data로 채운다. 그 후 기본 fine-lock mode에서는 약 2/5를 empty, 약 3/5를 valid로 표시한다 (`src/sat.cc:415`, `src/sat.cc:526`).

empty block이 필요한 이유:

- 각 CopyThread가 destination 하나 필요
- file/network read가 들어갈 destination 필요
- worker끼리 destination 경쟁을 줄이기 위함

모든 block은 이미 pattern으로 한 번 채워졌기 때문에 empty로 표시된 block에도 물리적으로 이전 데이터가 남아 있을 수 있다. 논리적으로만 버린 것이다.

## Coarse-grain queue

`--coarse_grain_lock`은 legacy `PageEntryQueue` 두 개를 사용한다.

- `empty_` queue
- `valid_` queue
- 각 queue마다 global mutex 하나
- random entry를 next-out 위치와 swap한 뒤 pop

동일한 correctness semantics를 유지하지만 많은 worker에서 lock contention이 커질 수 있다. 성능 비교/benchmarking 목적의 option이다.

## Lock과 correctness

worker가 block을 Get하면 해당 block은 Put할 때까지 다른 worker가 가져가지 못한다. 따라서 정상 경로에서는 두 CopyThread가 같은 destination block을 동시에 쓰지 않는다.

하지만 queue metadata 자체도 CPU cache line을 읽고 쓰므로 SAT의 memory traffic이 test data만으로 구성되지는 않는다. `-m 0 -c N`도 page_entry lock, counter, log 등의 작은 write traffic은 남는다.

## Tag와 region 선택

초기화 중 block 첫 PA를 generic region으로 분류하고 bit mask tag를 기록한다. `--local_numa` 또는 `--remote_numa`이면 worker가 특정 tag의 block만 선택한다.

현재 generic `OsLayer`의 region 계산은 mobile NUMA/LPDDR channel topology를 의미 있게 표현하지 못한다. mobile ARM에서는 vendor-specific 구현 없이 이 option을 실제 local/remote DRAM locality로 해석하지 않는 것이 안전하다.

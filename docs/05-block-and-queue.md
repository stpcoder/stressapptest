# 메모리 block과 queue

stressapptest는 큰 메모리 영역을 기본 1 MiB block으로 나누고, queue로 각 block의 상태를 관리합니다. worker는 queue에서 source와 destination block을 가져오고 작업이 끝나면 다시 반환합니다.

## SAT block의 크기와 역할

stressapptest는 전체 allocation을 `page_length_` 단위로 나눈다. 기본값은 1 MiB이며 `-p`로 변경할 수 있다.

```text
pages_ = test memory bytes / SAT block bytes
```

소스는 이 단위를 `page`로 명명한다. 문서에서는 Linux MMU page와 구분하기 위해 `SAT block`으로 표기한다.

<sub><em>SAT block: stressapptest queue가 source, destination 및 검증 상태를 관리하는 논리적 memory chunk입니다.</em></sub><br>
<sub><em>Linux page: kernel과 MMU가 virtual-to-physical mapping을 관리하는 최소 page 단위입니다.</em></sub>

| 개념 | 기본 크기 | 누가 관리하는가 |
|---|---:|---|
| SAT block | 1 MiB | stressapptest queue |
| checksum slice | 4 KiB | CrcCopy/CrcCheck loop |
| Linux page | 기기 설정 의존 | kernel/MMU |
| cache line | 통상 64 B | CPU/cache coherency |
| LPDDR row | device density/map 의존 | DMC/DRAM |

## 각 block의 정보를 담는 `page_entry`

각 SAT block은 다음 metadata로 표현된다 (`src/queue.h:38`).

> **파일:** `src/queue.h` · **구조체:** `page_entry` · **기준:** `73b9df2`

```cpp
struct page_entry {
  uint64 offset;
  void *addr;
  uint64 paddr;
  class Pattern *pattern;
  int32 tag;
  uint32 touch;
  uint64 ts;
  uint32 lastcpu;
  class Pattern *lastpattern;
};
```

**해석:** 실제 test data는 `page_entry` 내부에 저장되지 않습니다. `offset`과 `addr`이 allocation 안의 data 위치를 가리키고, `pattern`은 그 위치에 있어야 하는 기대 데이터의 정의를 가리킵니다. `pattern == NULL`은 empty, non-null은 valid 상태로 사용됩니다.

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

`paddr`에는 block 첫 virtual address에 대응하는 PA가 저장된다. block을 구성하는 나머지 Linux page의 physical 연속성은 별도로 확인해야 한다.

## Empty와 Valid 상태

### Valid

- `pattern != NULL`
- memory 내용이 해당 pattern이라고 기대됨
- source/check/file/network output으로 사용 가능

### Empty

- `pattern == NULL`
- destination으로 덮어써도 됨
- queue에서 destination으로 사용할 수 있는 상태
- mmap과 physical backing은 유지되는 상태

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

## 기본 queue인 FineLockPEQueue

> **파일:** `src/finelock_queue.cc` · **함수:** `FineLockPEQueue::GetRandomWithPredicateTag()` · **기준:** `73b9df2`

```cpp
uint64 first_try = GetRandom64() % q_size_;
uint64 next_try = 1;

for (uint64 i = 0; i < q_size_; i++) {
  uint64 index = (next_try + first_try) % q_size_;
  next_try = (a_ * next_try + c_) % modlength_;

  if (!(pred_func)(&pages_[index]))
    continue;
  if ((tag != kDontCareTag) && !(pages_[index].tag & tag))
    continue;
  if (pthread_mutex_trylock(&(pagelocks_[index])) == 0) {
    // lock 획득 뒤 상태를 다시 검사한다.
  }
}
```

**해석:** worker는 연속 block 번호를 단순 증가시키지 않습니다. random 시작점과 linear-congruential 진행값으로 후보를 탐색하고, valid/empty 조건과 tag 조건이 맞는 entry의 개별 mutex를 획득합니다. block 내부 접근은 순차적이지만 block 간 선택 순서는 pseudo-random입니다.

<sub><em>Linear-congruential progression: 이전 정수에 곱셈과 덧셈을 적용하고 modulus 연산으로 다음 후보 index를 생성하는 결정적 순회 방식입니다.</em></sub><br>
<sub><em>Predicate: 후보 entry가 valid, empty 또는 특정 tag 조건을 만족하는지 판정하는 함수입니다.</em></sub><br>
<sub><em>Pseudo-random: 초기 상태와 알고리즘이 같으면 다시 생성할 수 있는 결정적 수열이지만 실행 중에는 주소 선택이 분산되도록 사용하는 값입니다.</em></sub>

기본 구현은 `page_entry[]` array와 block별 mutex로 구성된다 (`src/finelock_queue.cc`). entry 선택 순서는 pseudo-random array 탐색으로 결정된다.

<sub><em>Mutex: 하나의 block metadata와 data access 권한을 한 worker에 배타적으로 부여하는 동기화 객체입니다.</em></sub><br>
<sub><em>Fine-grain locking: 각 block에 독립 lock을 배치하여 서로 다른 block의 동시 처리를 허용하는 방식입니다.</em></sub>

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

## 주소를 선택하는 단위

주소 선택에는 두 granularity가 있다.

```text
큰 단위: 어느 1 MiB block인가? → pseudo-random
작은 단위: block 안에서 어느 byte인가? → 앞에서 뒤로 순차
```

이 구성은 block 간 주소 선택을 분산하고, 각 block 내부에서는 연속 cache line stream을 생성한다. 연속 stream은 hardware prefetch와 burst transaction 형성에 유리한 access 형태를 제공한다.

access granularity는 1 MiB block 선택과 block 내부 순차 접근으로 구성된다. 따라서 cache-line 단위 random access와 단일 buffer 순차 access의 특성을 각각 그대로 적용할 수 없으며, PMU 결과는 이 두 단계의 주소 선택 구조를 기준으로 해석한다.

## Valid/Empty 비율

초기 fill은 모든 block을 먼저 valid data로 채운다. 그 후 기본 fine-lock mode에서는 약 2/5를 empty, 약 3/5를 valid로 표시한다 (`src/sat.cc:415`, `src/sat.cc:526`).

empty block이 필요한 이유:

- 각 CopyThread가 destination 하나 필요
- file/network read가 들어갈 destination 필요
- worker끼리 destination 경쟁을 줄이기 위함

모든 block은 초기 fill에서 pattern data가 기록된다. `empty` 전환은 pattern metadata를 해제하며, 이전 data byte를 지우거나 physical backing을 반환하지 않는다.

## 한 번에 크게 잠그는 queue

`--coarse_grain_lock`은 legacy `PageEntryQueue` 두 개를 사용한다.

- `empty_` queue
- `valid_` queue
- 각 queue마다 global mutex 하나
- random entry를 next-out 위치와 swap한 뒤 pop

동일한 correctness semantics를 유지한다. worker 수가 증가하면 global mutex의 lock contention도 증가할 수 있다. 이 option은 queue 성능 비교와 benchmarking에 사용한다.

## Queue 잠금이 필요한 이유

worker가 block을 Get하면 해당 block은 Put할 때까지 다른 worker가 가져가지 못한다. 따라서 정상 경로에서는 두 CopyThread가 같은 destination block을 동시에 쓰지 않는다.

queue operation은 page_entry, lock, counter 및 log metadata에 대한 CPU read/write를 포함한다. `-m 0 -c N` 구성에서도 이 metadata write traffic은 발생한다.

## Tag로 worker 영역 나누기

초기화 중 block 첫 PA를 generic region으로 분류하고 bit mask tag를 기록한다. `--local_numa` 또는 `--remote_numa`이면 worker가 특정 tag의 block만 선택한다.

현재 generic `OsLayer`의 region 계산에는 mobile NUMA/LPDDR channel topology 정보가 포함되지 않는다. mobile ARM에서 local/remote DRAM locality를 판정하려면 vendor topology를 반영한 platform-specific 구현이 필요하다.

<sub><em>Region tag: worker가 local/remote 조건으로 block을 선택할 때 사용하는 software bit mask입니다.</em></sub><br>
<sub><em>NUMA locality: CPU와 memory node 사이의 topology에 따라 access latency와 bandwidth가 달라지는 특성입니다.</em></sub>

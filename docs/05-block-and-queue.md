# 메모리 block 관리 방법

stressapptest는 테스트 메모리를 기본 1 MiB 크기의 block으로 나눕니다. Queue는 각 block이 읽을 데이터인지, 새 데이터를 쓸 공간인지 관리합니다. Worker는 queue에서 원본 block과 대상 block을 가져오고 작업이 끝나면 다시 반환합니다.

## SAT block의 크기와 역할

stressapptest는 전체 테스트 메모리를 `page_length_` 단위로 나눕니다. 기본값은 1 MiB이며 `-p`로 변경할 수 있습니다.

```text
block 수 = 전체 테스트 메모리 byte / SAT block byte
```

소스 코드에서는 이 단위를 `page`라고 부릅니다. 이 문서에서는 Linux의 page와 혼동하지 않도록 `SAT block`으로 표기합니다.

<sub><em>SAT block: stressapptest의 queue가 원본, 대상, 검사 상태를 관리하는 메모리 구역입니다.</em></sub>
<sub><em>Linux page: kernel과 MMU가 virtual-to-physical mapping을 관리하는 최소 page 단위입니다.</em></sub>

| 개념 | 기본 크기 | 누가 관리하는가 |
|---|---:|---|
| SAT block | 1 MiB | stressapptest queue |
| checksum 검사 구간 | 4 KiB | CrcCopy·CrcCheck 반복문 |
| Linux page | 기기 설정 의존 | kernel/MMU |
| cache line | 통상 64 B | CPU/cache coherency |
| LPDDR row | device density/map 의존 | DMC/DRAM |

## 각 block의 정보를 담는 `page_entry`

각 SAT block의 상태는 다음 `page_entry` 구조체에 기록됩니다 (`src/queue.h:38`).

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

**코드 설명:** 실제 테스트 데이터는 `page_entry` 안에 저장되지 않습니다. `offset`과 `addr`은 테스트 메모리 안에서 해당 block이 위치한 주소를 가리킵니다. `pattern`은 해당 block에 기록되어 있어야 하는 데이터 pattern을 가리킵니다. `pattern == NULL`이면 새 데이터를 쓸 수 있는 empty 상태이고, 값이 있으면 읽고 검사할 수 있는 valid 상태입니다.

| 항목 | 의미 |
|---|---|
| `offset` | 전체 테스트 메모리의 시작 위치에서 해당 block까지의 virtual address 차이 |
| `addr` | Worker가 해당 block에 접근할 때 사용하는 virtual address |
| `paddr` | block 첫 주소에서 계산한 진단용 physical address |
| `pattern` | 이 block에 있어야 하는 pattern. null이면 empty 상태 |
| `tag` | 메모리 구역 또는 NUMA 조건을 선택하는 bit mask |
| `touch` | valid block이 선택된 횟수를 기록하는 값 |
| `ts` | 마지막으로 `GetValid`가 실행된 시각 |
| `lastcpu` | 마지막 쓰기를 수행한 CPU |
| `lastpattern` | 마지막으로 읽었을 때 기록된 pattern |

`paddr`에는 block의 첫 virtual address에 대응하는 physical address만 저장됩니다. Block 안에 있는 나머지 Linux page가 physical address에서도 연속인지는 별도로 확인해야 합니다.

## 읽을 block과 쓸 block 구분

### Valid

- `pattern != NULL`인 상태입니다.
- 메모리 내용이 `pattern`이 가리키는 기대 데이터와 같아야 합니다.
- 복사의 원본, 데이터 검사, 파일·네트워크 출력에 사용할 수 있습니다.

### Empty

- `pattern == NULL`인 상태입니다.
- 새 데이터를 써도 되는 대상 block입니다.
- `mmap()`으로 확보한 virtual address와 연결된 physical page는 그대로 유지됩니다.

상태 전이는 다음과 같다.

```text
Empty
  │ 초기 데이터 또는 복사 데이터 쓰기
  ▼
Valid(pattern=P)
  │ 복사의 원본으로 사용
  ▼
Empty
```

`CheckThread`는 설정된 시험 시간이 남아 있으면 검사한 block을 다시 valid 상태로 반환합니다. 마지막 전체 검사에서는 block을 empty 상태로 바꾸면서 valid block을 모두 검사합니다.

## 기본 block 관리 구조: FineLockPEQueue

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

**코드 설명:** Worker는 block 번호를 0부터 순서대로 선택하지 않습니다. 임의의 시작 위치를 정한 뒤 linear-congruential 계산으로 다음 후보를 고릅니다. 후보가 valid·empty 상태와 tag 조건을 만족하면 해당 block의 mutex 획득을 시도합니다. Block을 고르는 순서는 pseudo-random이지만, 선택한 block 내부는 앞에서 뒤로 순차 접근합니다.

<sub><em>Linear-congruential progression: 이전 정수에 곱셈과 덧셈을 적용하고 modulus 연산으로 다음 후보 index를 생성하는 결정적 순회 방식입니다.</em></sub>
<sub><em>Predicate: 후보 entry가 valid, empty 또는 특정 tag 조건을 만족하는지 판정하는 함수입니다.</em></sub>
<sub><em>Pseudo-random: 초기 상태와 알고리즘이 같으면 다시 생성할 수 있는 결정적 수열이지만 실행 중에는 주소 선택이 분산되도록 사용하는 값입니다.</em></sub>

기본 구현은 `page_entry[]` 배열과 각 block에 대응하는 mutex로 구성됩니다 (`src/finelock_queue.cc`). 배열에서 block을 찾는 순서는 pseudo-random 계산으로 정합니다.

<sub><em>Mutex: 하나의 block 상태 정보와 데이터 접근 권한을 한 Worker에만 부여하는 동기화 객체입니다.</em></sub>
<sub><em>Fine-grain locking: 각 block에 독립 lock을 배치하여 서로 다른 block의 동시 처리를 허용하는 방식입니다.</em></sub>

```text
page_entry[0] + lock[0]
page_entry[1] + lock[1]
...
page_entry[N] + lock[N]
```

Block을 가져오는 순서는 다음과 같습니다.

1. 임의의 시작 index를 선택합니다.
2. Linear-congruential 계산으로 배열의 다음 후보를 정합니다.
3. 후보의 valid·empty 상태와 tag가 Worker의 요청에 맞는지 확인합니다.
4. 해당 block의 mutex에 `trylock`을 실행합니다.
5. 잠금에 성공한 block을 Worker에 전달합니다.

작업이 끝나면 변경된 상태 정보를 같은 배열 위치에 기록하고 mutex를 해제합니다.

장점:

- Queue 전체를 하나의 mutex로 잠글 때보다 잠금 대기가 줄어듭니다.
- 여러 Worker가 서로 다른 block을 동시에 처리할 수 있습니다.
- 시작 위치와 탐색 순서가 바뀌므로 특정 block만 반복해서 선택하는 현상이 줄어듭니다.

## Block과 block 내부 주소 선택

주소는 다음 두 단계로 선택합니다.

```text
1단계: 처리할 1 MiB block 선택 → pseudo-random
2단계: 선택한 block 내부 처리 → 앞에서 뒤로 순차 접근
```

이 방식은 전체 테스트 메모리에서 처리할 block을 분산해서 고르는 동시에, 선택한 block 안에서는 연속된 cache line을 처리합니다. 연속 접근은 hardware prefetch가 다음 데이터를 미리 요청하고 memory controller가 여러 요청을 연속해서 처리할 수 있게 합니다.

따라서 stressapptest는 모든 cache line을 완전히 임의 순서로 접근하는 시험도 아니고, 하나의 큰 buffer를 처음부터 끝까지 순차 접근하는 시험도 아닙니다. PMU 결과는 `block은 분산 선택, block 내부는 순차 접근`이라는 구조를 기준으로 해석해야 합니다.

## 읽을 block과 쓸 block의 비율

초기 데이터 쓰기 단계에서는 모든 block을 valid 데이터로 채웁니다. 이후 기본 fine-lock 방식에서는 전체 block의 약 2/5를 empty, 약 3/5를 valid 상태로 설정합니다 (`src/sat.cc:415`, `src/sat.cc:526`).

empty block이 필요한 이유:

- 각 `CopyThread`가 데이터를 쓸 대상 block이 필요합니다.
- 파일·네트워크에서 읽은 데이터를 저장할 대상 block이 필요합니다.
- 여러 Worker가 대상 block을 얻기 위해 대기하는 시간을 줄입니다.

모든 block에는 초기 단계에서 pattern 데이터가 기록됩니다. Empty 상태로 바꿀 때에는 `pattern` 정보만 제거합니다. 이전 데이터 byte를 지우거나 연결된 physical page를 운영체제에 반환하지는 않습니다.

## 전체 queue를 한 번에 잠그는 방식

`--coarse_grain_lock`은 이전 방식의 `PageEntryQueue` 두 개를 사용합니다.

- `empty_` queue
- `valid_` queue
- 각 queue 전체를 보호하는 mutex 하나
- 임의로 선택한 항목을 꺼낼 위치와 교환한 뒤 queue에서 제거

Block의 상태 관리 규칙은 기본 방식과 같습니다. 그러나 Worker 수가 늘면 여러 Worker가 queue 전체의 mutex를 기다리는 시간이 증가할 수 있습니다. 이 옵션은 두 queue 구현의 성능을 비교할 때 사용합니다.

## Queue 잠금이 필요한 이유

Worker가 block을 가져오면 작업을 끝내고 반환할 때까지 다른 Worker는 그 block을 사용할 수 없습니다. 따라서 정상 동작에서는 두 `CopyThread`가 같은 대상 block에 동시에 쓰지 않습니다.

Queue 처리 과정에서도 CPU는 `page_entry`, mutex, counter, 로그 정보를 읽고 씁니다. 따라서 `-m 0 -c N`처럼 `CopyThread`를 사용하지 않는 구성에서도 이 관리 정보에 대한 소량의 메모리 접근은 발생합니다.

## Tag를 이용한 block 구역 선택

초기화 과정에서 block 첫 physical address를 공통 규칙에 따라 메모리 구역으로 분류하고 bit mask tag를 기록합니다. `--local_numa` 또는 `--remote_numa`를 사용하면 Worker는 조건에 맞는 tag의 block만 선택합니다.

현재 공통 `OsLayer`의 메모리 구역 계산에는 모바일 SoC의 NUMA 또는 LPDDR channel 구조가 포함되어 있지 않습니다. 모바일 ARM에서 CPU와 DRAM의 local·remote 관계를 구분하려면 SoC 제조사의 구조를 반영한 별도 구현이 필요합니다.

<sub><em>Region tag: worker가 local/remote 조건으로 block을 선택할 때 사용하는 software bit mask입니다.</em></sub>
<sub><em>NUMA locality: CPU와 memory node 사이의 topology에 따라 access latency와 bandwidth가 달라지는 특성입니다.</em></sub>

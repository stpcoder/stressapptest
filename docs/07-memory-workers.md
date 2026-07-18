# 메모리 worker 동작

## 공통 WorkerThread 구조

모든 worker는 `WorkerThread`를 상속하며 다음 상태를 가진다 (`src/worker.h:204`).

- thread id와 `pthread_t`
- 실행 가능한 CPU mask
- queue region tag
- page/block 처리 횟수
- error count와 status
- start time과 run duration
- shared `Sat`, `OsLayer`, `PatternList` pointer
- pause/stop을 관리하는 `WorkerStatus`

실제 OS thread는 `pthread_create()`로 만들어지고 시작 시 가능한 경우 `sched_setaffinity()`를 적용한다.

## FillThread

### 생성 시점

초기 memory initialization 단계에서 기본 8개 생성된다. timed `-s` 구간 이전에 끝나고 삭제된다.

### 동작

```text
GetEmpty(block)
 → weighted random Pattern 선택
 → 1 MiB 전체를 64-bit word로 write
 → lastcpu 기록
 → PutValid(block)
```

Tag mode가 아니면 각 64-bit store는 pattern의 32-bit word 두 개를 조합한다.

```cpp
data.low  = pattern(index)
data.high = pattern(index + 1)
mem64[i]  = data
```

### traffic 성격

- test memory 전체를 한 번 쓰는 write-heavy phase
- anonymous mmap의 first touch/page fault 동반
- 일반 cached store이므로 write allocate와 dirty eviction 가능
- 최종 LPDDR write command 수는 cache/DMC 정책에 의존

순수 write-only worker는 아니지만 초기 fill이 가장 write 중심에 가까운 phase다.

## CopyThread

### 기본 개수

`-m` 미지정 시 online logical CPU 수다. `-m N`으로 지정한다.

### block 상태 전이

```text
valid source(P) + empty destination
                 ↓ copy
empty old source + valid destination(P)
```

각 worker는 고정 block을 갖지 않고 매 loop마다 queue에서 random block을 다시 선택한다.

### 세 가지 copy mode

#### 기본 strict mode

```text
CrcCopyPage()
 = source load + checksum + destination store
```

4 KiB slice마다 source checksum을 expected checksum과 비교한다.

#### `-W` warm mode

```text
CrcWarmCopyPage()
 = vector/CPU-intensive copy + checksum
```

현재 ARM64 master에서는 `prfm`, `ld1`, `st1`, vector add를 사용하는 64 B loop다. 일반 cached NEON access다.

#### `-F` fast mode

```text
libc memcpy(destination, source, 1 MiB)
```

transaction checksum을 생략한다. destination pattern metadata는 복사하지만 bytes를 즉시 검사하지 않는다.

`-W`와 `-F`를 동시에 주면 `CopyThread::Work()`의 분기 순서상 `-W`가 우선한다. 이 조합은 libc `memcpy()` mode가 아니라 warm checksum copy mode로 실행된다.

### loop 종료 시 yield

1 MiB copy가 끝나면 `sched_yield()`한다. inner copy 중간에 context switch되어 cache가 불필요하게 thrash하는 것을 줄이고 여러 worker가 block 단위로 협력하도록 의도한 동작이다.

### 논리적 bandwidth

CopyThread는 한 block마다 `block bytes × 2`를 memory copied data로 보고한다.

```text
1 MiB source read + 1 MiB destination write = 2 MiB logical
```

이는 write allocate, prefetch, refill, checksum reread, cache writeback까지 포함한 DMC byte가 아니다.

## CheckThread

### 옵션으로 실행

`-c N`이면 timed run 동안 N개 CheckThread가 동작한다.

```text
GetValid(block)
 → 4 KiB 단위 CrcCheckPage
 → 실행 중이면 PutValid
```

test data 기준 read-only지만 queue lock, counter, log와 metadata write는 존재한다.

### Final check

timed worker가 끝난 뒤 기본 8개 CheckThread가 남은 valid block을 검사한다. 이때는 검사한 block을 empty로 바꿔 valid pool을 완전히 소진한다.

### traffic 성격

- source read/checksum 중심
- destination data write 없음
- 큰 working set이면 cache refill/LPDDR read가 발생
- prefetch와 SLC hit에 따라 실제 DMC read가 달라짐

`-m 0 -c N`은 SAT에서 read-only에 가장 가까운 구성이다.

## InvertThread

### 옵션

`-i N`으로 생성한다. 기본값은 0이다.

### 한 block의 sequence

strict mode라면 시작 전 checksum 검사를 한다. 이후:

```text
InvertUp      : 낮은 주소 → 높은 주소, 각 32-bit bitwise NOT
yield
InvertDown    : 높은 주소 → 낮은 주소, NOT
yield
InvertDown    : 높은 주소 → 낮은 주소, NOT
yield
InvertUp      : 낮은 주소 → 높은 주소, NOT
yield
```

마지막에 strict checksum을 다시 검사한다.

각 word가 네 번 반전되므로 최종 bytes는 원래 pattern으로 돌아온다.

### access 성격

`x = ~x`이므로 각 word마다:

```text
read → invert ALU → write
```

64 B마다 `FastFlushHint()`도 호출한다. ARM64에서는 `dc cvau`와 D/I cache maintenance sequence가 실행되므로 barrier/cache-maintenance overhead가 크다.

### 논리적 bandwidth 주의

코드는 InvertThread 처리량을 `GetCopiedData() × 4`로 보고한다. 실제로는 four-pass RMW, optional pre/post checksum, cache maintenance가 있으므로 DMC byte와 직접 대응하지 않는다.

## MemoryRegionThread

`MemoryRegionThread` class는 별도의 memory/MMIO region에 SAT pattern을 copy하고 check할 수 있도록 구현되어 있다.

```text
SAT valid source
 → 지정 region block으로 CrcCopy
 → 지정 region CrcCheck
 → source와 region block 반환
```

그러나 public `Sat::InitializeThreads()`는 이 class를 생성하지 않으며 해당 command-line option도 없다. platform-specific integration이나 code extension을 위한 class로 봐야 한다.

일반 실행에서 `--paddr_base`가 이 worker를 자동으로 활성화하지 않는다.

## Traffic 관점 비교

| Worker/mode | test data read | test data write | 즉시 checksum | 주요 목적 |
|---|---:|---:|---:|---|
| FillThread | 낮음/WA 의존 | 큼 | 없음 | 초기 pattern/first touch |
| CopyThread 기본 | 큼 | 큼 | source | mixed traffic + correctness |
| CopyThread `-W` | 큼 | 큼 | source | SIMD/CPU+memory load |
| CopyThread `-F` | 큼 | 큼 | 없음 | 높은 memcpy throughput |
| CheckThread | 큼 | 없음 | source | read verification |
| InvertThread | 매우 큼 | 매우 큼 | 전/후 | RMW/march-like 방향 전환 |

WA는 write allocate를 의미한다. “없음”은 test data array 기준이며 metadata write까지 없다는 뜻은 아니다.

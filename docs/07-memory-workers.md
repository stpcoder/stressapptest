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

> **파일:** `src/worker.cc` · **함수:** `WorkerThread::SpawnThread()` · **기준:** `73b9df2`

```cpp
int WorkerThread::SpawnThread() {
  int result = pthread_create(&thread_, NULL, thread_spawner_, this);
  if (result) {
    status_ = false;
    return false;
  }
  return true;
}
```

**해석:** worker object 하나가 POSIX thread 하나에 대응합니다. 기본 `-m` 값은 online CPU 수이므로 CopyThread 수가 CPU 수와 같아집니다. FillThread 8개, ErrorPollThread, option으로 활성화한 다른 worker는 별도로 추가되므로 전체 thread 수가 core 수와 항상 같지는 않습니다.

<sub><em>WorkerThread: 공통 thread 상태, CPU mask, 오류 수 및 실행 시간을 관리하는 base class입니다.</em></sub><br>
<sub><em>CPU affinity: thread가 실행될 수 있는 CPU 집합을 scheduler에 지정하는 속성입니다.</em></sub>

## FillThread

> **파일:** `src/worker.cc` · **함수:** `FillThread::Work()` · **기준:** `73b9df2`

```cpp
while (IsReadyToRun() && (loops < num_pages_to_fill_)) {
  result = result && sat_->GetEmpty(&pe);
  if (!result)
    break;

  result = result && FillPageRandom(&pe);
  if (!result)
    break;

  result = result && sat_->PutValid(&pe);
  if (!result)
    break;
  loops++;
}
```

**해석:** empty block을 하나 획득하여 weighted random pattern으로 전체 block을 채운 후 valid 상태로 반환합니다. 초기 fill은 8개 FillThread가 전체 block 범위를 나누어 완료할 때까지 실행됩니다.

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

일반 pattern mode에서 각 64-bit store는 pattern의 32-bit word 두 개를 조합한다. Tag mode에서는 각 64 B line의 첫 8 B에 address tag를 기록한다.

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

초기 fill은 기존 pattern data를 source로 읽지 않고 test range 전체에 store를 수행하는 write 중심 phase다. page fault 처리와 write-allocate가 필요한 platform에서는 page-table 및 cache-line read transaction이 추가될 수 있다.

<sub><em>Write-allocate: store miss에서 cache line과 write ownership을 확보한 후 cache에서 수정하는 정책입니다.</em></sub>

## CopyThread

> **파일:** `src/worker.cc` · **함수:** `CopyThread::Work()` · **기준:** `73b9df2`

```cpp
while (IsReadyToRun()) {
  result = result && sat_->GetValid(&src, tag_);
  result = result && sat_->GetEmpty(&dst, tag_);

  if (sat_->warm())
    CrcWarmCopyPage(&dst, &src);
  else if (sat_->strict())
    CrcCopyPage(&dst, &src);
  else
    memcpy(dst.addr, src.addr, sat_->page_length());

  result = result && sat_->PutValid(&dst);
  result = result && sat_->PutEmpty(&src);
  YieldSelf();
}
```

**해석:** valid source와 empty destination을 동시에 확보한 뒤 한 SAT block 전체를 처리합니다. destination은 valid가 되고 기존 source는 empty가 됩니다. `-W`, 기본 strict, `-F` 경로의 우선순위도 이 조건문에서 확정됩니다.

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

transaction checksum을 생략한다. destination pattern metadata를 복사하고 destination byte 검사는 이후 source 선택 또는 final check에서 수행한다.

`-W`와 `-F`를 동시에 지정하면 `CopyThread::Work()`의 첫 번째 조건인 `warm()` 분기가 선택된다. 실행 경로는 `CrcWarmCopyPage()`이며 libc `memcpy()` 분기는 실행되지 않는다.

### loop 종료 시 yield

1 MiB copy가 끝나면 `sched_yield()`한다. inner copy 중간에 context switch되어 cache가 불필요하게 thrash하는 것을 줄이고 여러 worker가 block 단위로 협력하도록 의도한 동작이다.

### 논리적 bandwidth

CopyThread는 한 block마다 `block bytes × 2`를 memory copied data로 보고한다.

```text
1 MiB source read + 1 MiB destination write = 2 MiB logical
```

이 값은 software가 처리한 source와 destination byte 합계다. DMC byte에는 write allocate, prefetch, refill, checksum reread 및 cache write-back에 의해 추가되거나 cache hit으로 제거되는 transaction이 반영된다.

<sub><em>Logical bandwidth: software가 처리 block 수와 block 크기로 계산한 전송률입니다.</em></sub><br>
<sub><em>DMC bandwidth: DRAM memory controller가 실제로 처리한 read/write 데이터 전송률입니다.</em></sub>

## CheckThread

> **파일:** `src/worker.cc` · **함수:** `CheckThread::Work()` · **기준:** `73b9df2`

```cpp
while (true) {
  result = result && sat_->GetValid(&pe);
  if (!result)
    break;

  CrcCheckPage(&pe);

  if (IsReadyToRunNoPause())
    result = result && sat_->PutValid(&pe);
  else
    result = result && sat_->PutEmpty(&pe);
  loops++;
}
```

**해석:** timed 구간에서는 검사한 block을 다시 valid로 반환하여 반복 검사합니다. final check에서는 stop 상태로 시작하므로 검사 완료 block을 empty로 전환하고 valid block이 소진될 때까지 진행합니다.

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

`-m 0 -c N` 구성은 timed 구간의 test data access를 read/checksum으로 제한한다. queue lock, counter 및 metadata write는 유지된다.

## InvertThread

> **파일:** `src/worker.cc` · **함수:** `InvertThread::Work()` · **기준:** `73b9df2`

```cpp
if (sat_->strict())
  CrcCheckPage(&src);

InvertPageUp(&src);
YieldSelf();
InvertPageDown(&src);
YieldSelf();
InvertPageDown(&src);
YieldSelf();
InvertPageUp(&src);
YieldSelf();

if (sat_->strict())
  CrcCheckPage(&src);
```

**해석:** 한 transaction에서 상승 방향과 하강 방향의 bitwise inversion을 각각 두 번 수행합니다. 네 pass 이후 데이터는 원래 pattern으로 복원되며 strict mode는 작업 전후 checksum을 확인합니다.

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

> **파일:** `src/worker.cc` · **함수:** `MemoryRegionThread::Work()` · **기준:** `73b9df2`

```cpp
result = result && sat_->GetValid(&source_pe);
result = result && pages_->PopRandom(&memregion_pe);

phase_ = kPhaseCopy;
CrcCopyPage(&memregion_pe, &source_pe);
memregion_pe.pattern = source_pe.pattern;
memregion_pe.lastcpu = sched_getcpu();

phase_ = kPhaseCheck;
CrcCheckPage(&memregion_pe);

result = result && sat_->PutValid(&source_pe);
result = result && pages_->Push(&memregion_pe);
```

**해석:** 일반 SAT valid block을 별도로 등록된 memory/MMIO region block으로 copy하고 해당 region에서 checksum을 검사합니다. public `Sat::InitializeThreads()`에는 이 class를 생성하는 option 경로가 없으므로 별도 integration이 필요합니다.

`MemoryRegionThread` class는 별도의 memory/MMIO region에 SAT pattern을 copy하고 check할 수 있도록 구현되어 있다.

```text
SAT valid source
 → 지정 region block으로 CrcCopy
 → 지정 region CrcCheck
 → source와 region block 반환
```

public `Sat::InitializeThreads()`에는 이 class의 생성 코드가 없으며 command-line option도 제공되지 않는다. 이 class는 platform-specific integration 또는 code extension에서 명시적으로 연결해야 실행된다.

일반 실행에서 `--paddr_base`가 이 worker를 자동으로 활성화하지 않는다.

## Traffic 관점 비교

| Worker/mode | test data read | test data write | 즉시 checksum | 주요 목적 |
|---|---:|---:|---:|---|
| FillThread | 낮음/WA 의존 | 큼 | 없음 | 초기 pattern/first touch |
| CopyThread 기본 | 큼 | 큼 | source | mixed traffic + correctness |
| CopyThread `-W` | 큼 | 큼 | source | SIMD/CPU+memory load |
| CopyThread `-F` | 큼 | 큼 | 없음 | 높은 memcpy throughput |
| CheckThread | 큼 | 없음 | source | read verification |
| InvertThread | four-pass RMW | four-pass RMW | 전/후 | RMW/march-like 방향 전환 |

WA는 write allocate를 의미한다. 표의 “없음”은 test data array에 대한 해당 access가 0임을 의미하며 queue와 worker metadata write는 별도로 발생한다.

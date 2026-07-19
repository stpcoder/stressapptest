# 메모리 Worker 종류와 동작

메모리 Worker는 같은 queue에서 block을 가져오지만 서로 다른 작업을 수행합니다. `FillThread`는 초기 데이터를 쓰고, `CopyThread`는 데이터를 읽고 검사하면서 다른 block에 복사하며, `CheckThread`는 기록된 데이터가 기대값과 같은지 검사합니다.

## Worker의 공통 동작

모든 Worker는 `WorkerThread`를 상속하며 다음 정보를 관리합니다 (`src/worker.h:204`).

- Worker 번호와 `pthread_t`
- 실행 가능한 CPU mask
- queue region tag
- 처리한 block 수
- 오류 수와 실행 상태
- 시작 시각과 실행 시간
- 공동으로 사용하는 `Sat`, `OsLayer`, `PatternList` 객체의 주소
- 일시 정지와 종료를 관리하는 `WorkerStatus`

실제 OS thread는 `pthread_create()`로 생성합니다. 실행을 시작할 때 CPU affinity를 설정할 수 있으면 `sched_setaffinity()`를 적용합니다.

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

**코드 설명:** Worker 객체 하나가 POSIX thread 하나에 대응합니다. `-m`을 지정하지 않으면 현재 online 상태인 logical CPU 수만큼 `CopyThread`를 만듭니다. 초기화에 사용하는 `FillThread` 8개, `ErrorPollThread`, 다른 옵션으로 추가한 Worker는 별도로 생성됩니다. 따라서 전체 thread 수가 CPU core 수와 항상 같은 것은 아닙니다.

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

**코드 설명:** Empty block 하나를 가져와 선택 비율에 따라 고른 pattern으로 전체 block을 채운 뒤 valid 상태로 반환합니다. 초기 데이터 쓰기는 8개의 `FillThread`가 전체 block을 처리할 때까지 진행됩니다.

### 생성 시점

테스트 메모리를 초기화할 때 기본 8개를 생성합니다. `-s`로 지정한 본 시험 시간이 시작되기 전에 작업을 마치고 삭제됩니다.

### 동작

```text
GetEmpty(block)
 → 선택 비율에 따라 Pattern 선택
 → 1 MiB 전체에 64-bit 단위로 쓰기
 → 마지막으로 쓴 CPU 기록
 → PutValid(block)
```

일반 pattern 방식에서는 32-bit pattern 두 개를 조합하여 64-bit 값을 씁니다. Tag mode에서는 각 64 B cache line의 첫 8 B에 주소 tag를 기록합니다.

```cpp
data.low  = pattern(index)
data.high = pattern(index + 1)
mem64[i]  = data
```

### 메모리 접근 특징

- 전체 테스트 메모리를 한 번 쓰는 단계입니다.
- anonymous `mmap()` 영역에 처음 접근하므로 page fault와 physical page 할당이 발생합니다.
- 일반 cacheable 쓰기를 사용하므로 write allocate와 dirty cache line 교체가 발생할 수 있습니다.
- 실제 LPDDR 쓰기 명령 수는 cache와 DMC 정책에 따라 달라집니다.

초기 데이터 쓰기 단계는 기존 pattern을 원본으로 읽지 않고 테스트 범위 전체에 새 값을 씁니다. Page fault 처리와 write allocate가 필요한 기기에서는 page table과 cache line을 읽는 요청도 함께 발생할 수 있습니다.

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

**코드 설명:** Valid 원본 block과 empty 대상 block을 하나씩 가져와 SAT block 전체를 복사합니다. 작업이 끝나면 대상은 valid 상태가 되고 기존 원본은 empty 상태가 됩니다. 이 조건문은 `-W`, 기본 검사 복사, `-F`의 실행 우선순위도 결정합니다.

### 생성 개수

`-m`을 지정하지 않으면 현재 online 상태인 logical CPU 수만큼 생성합니다. `-m N`으로 개수를 직접 지정할 수 있습니다.

### 복사 전후의 block 상태

```text
valid 원본(P) + empty 대상
                 ↓ 복사
empty가 된 기존 원본 + valid가 된 대상(P)
```

각 Worker는 고정된 block을 사용하지 않습니다. 반복할 때마다 queue에서 원본과 대상을 다시 선택합니다.

### 세 가지 복사 방식

#### 기본 검사 복사

```text
CrcCopyPage()
 = 원본 읽기 + checksum 계산 + 대상 쓰기
```

원본을 4 KiB씩 나누어 계산한 checksum을 기대 checksum과 비교합니다.

#### `-W`: ARM64 vector 복사

```text
CrcWarmCopyPage()
 = vector 명령을 사용한 복사 + checksum 계산
```

현재 분석한 ARM64 코드에서는 `prfm`, `ld1`, `st1`, vector add를 사용하여 한 번에 64 B씩 처리합니다. Cache를 우회하지 않는 일반 NEON 읽기·쓰기입니다.

#### `-F`: libc `memcpy()` 복사

```text
libc memcpy(destination, source, 1 MiB)
```

복사 중 checksum 계산을 생략합니다. 대상 block에는 원본의 pattern 정보를 전달합니다. 대상 데이터 검사는 이후 이 block을 원본으로 읽거나 마지막 전체 검사를 수행할 때 이루어집니다.

`-W`와 `-F`를 동시에 지정하면 첫 번째 조건인 `warm()`이 우선 적용됩니다. 따라서 `CrcWarmCopyPage()`가 실행되고 C library의 `memcpy()`는 실행되지 않습니다.

### Block 복사 후 CPU 실행권 양보

1 MiB 복사가 끝나면 `sched_yield()`를 호출하여 다른 thread가 실행될 기회를 줍니다. 하나의 block을 처리하는 도중에 thread가 자주 바뀌어 cache 내용이 교체되는 현상을 줄이기 위한 동작입니다.

### 처리량 계산

`CopyThread`는 한 block마다 `block byte × 2`를 처리량으로 계산합니다.

```text
원본 1 MiB 읽기 + 대상 1 MiB 쓰기 = 논리적 처리량 2 MiB
```

이 값은 프로그램이 처리한 원본과 대상의 byte를 더한 수치입니다. 실제 DMC 처리량은 write allocate, prefetch, cache refill, 오류 재검사, cache write-back, cache hit의 영향을 받으므로 이 값과 다를 수 있습니다.

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

**코드 설명:** 설정한 시험 시간이 남아 있으면 검사한 block을 다시 valid 상태로 반환하여 이후에도 사용할 수 있게 합니다. 마지막 전체 검사에서는 완료한 block을 empty 상태로 바꾸며 valid block이 없어질 때까지 검사합니다.

### 실행 중 검사 Worker

`-c N`을 지정하면 본 시험이 진행되는 동안 N개의 `CheckThread`가 동작합니다.

```text
GetValid(block)
 → 4 KiB 단위 CrcCheckPage
 → 실행 중이면 PutValid
```

테스트 데이터는 읽기만 합니다. 다만 queue의 mutex, 처리 횟수, 로그, block 상태 정보에는 쓰기가 발생합니다.

### 종료 후 전체 검사

본 시험의 Worker가 모두 끝나면 8개의 `CheckThread`가 남은 valid block을 검사합니다. 검사한 block을 empty 상태로 바꾸면서 모든 valid block을 처리합니다.

### 메모리 접근 특징

- 원본 데이터를 읽고 checksum을 계산합니다.
- 대상 데이터 쓰기는 없습니다.
- 검사 범위가 cache보다 크면 cache refill과 LPDDR 읽기가 발생합니다.
- 실제 DMC 읽기량은 prefetch와 SLC hit 비율에 따라 달라집니다.

`-m 0 -c N`으로 실행하면 본 시험 중 테스트 데이터 접근은 읽기와 checksum 계산이 중심이 됩니다. Queue mutex, 처리 횟수, block 상태 정보에 대한 쓰기는 계속 발생합니다.

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

**코드 설명:** 낮은 주소에서 높은 주소 방향과 반대 방향으로 bit 반전을 각각 두 번 수행합니다. 전체 block을 네 번 처리하면 데이터는 원래 pattern으로 돌아옵니다. 기본 검사 방식에서는 반전 작업 전후에 checksum도 확인합니다.

### 옵션

`-i N`으로 개수를 지정합니다. 기본값은 0이므로 옵션을 지정하지 않으면 생성하지 않습니다.

### 한 block의 처리 순서

기본 검사 방식에서는 시작하기 전에 checksum을 검사합니다. 이후 다음 순서로 동작합니다.

```text
InvertUp      : 낮은 주소 → 높은 주소, 각 32-bit bitwise NOT
다른 thread에 실행 기회 양보
InvertDown    : 높은 주소 → 낮은 주소, NOT
yield
InvertDown    : 높은 주소 → 낮은 주소, NOT
yield
InvertUp      : 낮은 주소 → 높은 주소, NOT
yield
```

마지막에 checksum을 다시 검사합니다.

각 word를 네 번 반전하므로 마지막 데이터는 원래 pattern으로 돌아옵니다.

### 메모리 접근 특징

`x = ~x`이므로 각 word마다:

```text
읽기 → CPU에서 bit 반전 → 같은 주소에 쓰기
```

64 B마다 `FastFlushHint()`도 호출합니다. ARM64에서는 `dc cvau`와 data·instruction cache 관리 명령이 실행되므로 barrier와 cache 관리 명령의 실행 시간이 추가됩니다.

### 처리량 계산 시 주의점

프로그램은 `InvertThread` 처리량을 `GetCopiedData() × 4`로 계산합니다. 실제 동작에는 네 번의 read-modify-write, 작업 전후 checksum, cache 관리 명령이 포함될 수 있으므로 이 값은 DMC byte와 직접 일치하지 않습니다.

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

**코드 설명:** 일반 SAT valid block의 데이터를 별도로 등록한 memory 또는 MMIO 영역으로 복사하고, 그 영역의 checksum을 검사합니다. 공개 코드의 `Sat::InitializeThreads()`에는 이 Worker를 생성하는 명령 옵션이 없으므로 별도 코드 연결이 필요합니다.

`MemoryRegionThread`는 별도의 memory 또는 MMIO 영역에 SAT pattern을 복사하고 검사하도록 구현되어 있습니다.

```text
SAT valid 원본
 → 지정한 메모리 구역의 block으로 복사
 → 지정한 메모리 구역에서 검사
 → 원본과 메모리 구역 block 반환
```

공개 `Sat::InitializeThreads()`에는 이 Worker의 생성 코드가 없고 명령 옵션도 제공하지 않습니다. SoC별 코드 또는 별도 확장 코드에서 명시적으로 생성해야 실행됩니다.

일반 실행에서 `--paddr_base`를 지정해도 이 Worker가 자동으로 활성화되지는 않습니다.

## Worker별 읽기·쓰기 비교

| Worker 또는 실행 방식 | 테스트 데이터 읽기 | 테스트 데이터 쓰기 | 즉시 checksum 검사 | 주요 목적 |
|---|---:|---:|---:|---|
| FillThread | write allocate에 따라 발생 | 큼 | 없음 | 초기 pattern 기록과 physical page 할당 |
| CopyThread 기본 | 큼 | 큼 | 원본 | 읽기·쓰기 부하와 데이터 검사 |
| CopyThread `-W` | 큼 | 큼 | 원본 | SIMD 연산과 메모리 부하 |
| CopyThread `-F` | 큼 | 큼 | 없음 | 높은 메모리 복사 처리량 |
| CheckThread | 큼 | 없음 | 원본 | 읽기 중심 데이터 검사 |
| InvertThread | 네 번의 RMW | 네 번의 RMW | 작업 전·후 | 양방향 read-modify-write |

표의 “없음”은 테스트 데이터 영역에 해당 접근이 없다는 의미입니다. Queue와 Worker의 상태 정보에 대한 쓰기는 별도로 발생합니다.

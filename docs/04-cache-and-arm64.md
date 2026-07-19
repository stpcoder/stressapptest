# Cache에서 LPDDR까지 데이터가 이동하는 과정

stressapptest는 Android가 제공한 일반 cacheable memory를 사용합니다. cache를 끄거나 LPDDR을 직접 읽고 쓰지는 않습니다. 테스트 영역을 cache 용량보다 크게 설정하고 여러 Worker가 반복해서 접근하면 cache miss와 dirty cache line의 교체가 늘어납니다. 그 결과 하위 cache와 LPDDR로 전달되는 읽기·쓰기 요청이 증가합니다.

## Stressapptest의 cache 사용 방식

stressapptest는 테스트 메모리의 page table에 설정된 cache 속성을 변경하지 않습니다. 실제 cache 동작은 다음 세 가지 요소로 결정됩니다.

1. **운영체제가 정하는 메모리 속성**: kernel의 page table과 MAIR가 Normal, Cacheable, Write-Back, Shareable 속성을 정합니다.
2. **CPU가 정하는 cache 내부 동작**: CPU가 cache line 교체, prefetch, write allocation, streaming 최적화, write-back 시점을 정합니다.
3. **stressapptest가 만드는 접근 방식**: stressapptest가 block 크기, 주소 선택, 읽기·쓰기 순서, Worker 수를 정합니다.

stressapptest가 직접 구성하는 부분은 세 번째 항목입니다.

<sub><em>Memory attribute policy: kernel이 page table과 MAIR를 통해 memory type, cacheability 및 shareability를 지정하는 정책입니다.</em></sub><br>
<sub><em>Microarchitecture policy: CPU 구현이 replacement, prefetch, write allocation 및 write-back 시점을 결정하는 내부 정책입니다.</em></sub><br>
<sub><em>Workload access policy: software가 주소, 크기, 순서, read/write 비율 및 thread 수를 구성하는 방식입니다.</em></sub>

## ARM64 cache 관련 코드

> **파일:** `src/os.h` · **함수:** `OsLayer::FastFlush()` · **기준:** `73b9df2`

```cpp
#elif defined(STRESSAPPTEST_CPU_AARCH64)
  asm volatile("dc cvau, %0" : : "r" (vaddr));
  asm volatile("dsb ish");
  asm volatile("ic ivau, %0" : : "r" (vaddr));
  asm volatile("dsb ish");
  asm volatile("isb");
#endif
```

**코드 설명:** `dc cvau`는 해당 virtual address의 data cache line을 Point of Unification 방향으로 clean합니다. `ic ivau`는 같은 주소의 instruction cache line을 무효화합니다. 이 조합은 data cache line을 Point of Coherency까지 clean한 뒤 무효화하는 `dc civac`와 다릅니다. 따라서 ARM64의 `FastFlush()`가 데이터를 LPDDR까지 즉시 쓰거나 data cache를 완전히 비운다고 해석하면 안 됩니다.

<sub><em>Point of Unification, PoU: instruction fetch와 data access가 동일한 memory copy를 관찰하도록 합류하는 cache hierarchy 지점입니다.</em></sub><br>
<sub><em>Point of Coherency, PoC: 해당 shareability domain의 CPU와 coherent observer가 동일한 memory copy를 관찰하는 지점입니다.</em></sub><br>
<sub><em>Cache clean: dirty data를 지정된 cache hierarchy 지점 방향으로 기록하고 line의 valid 상태는 유지할 수 있는 동작입니다.</em></sub><br>
<sub><em>Cache invalidate: cache line의 valid 상태를 제거하여 이후 access가 다시 line을 획득하게 하는 동작입니다.</em></sub>

## CPU가 쓴 데이터가 LPDDR에 반영되는 순서

```text
CPU가 데이터 쓰기
  ↓
store buffer
  ↓
L1D cache line이 수정되어 dirty 상태가 됨
  ↓ cache line 교체·clean·coherency 처리
L2 또는 cluster 단위 cache
  ↓
SLC/LLCC
  ↓
DMC write queue
  ↓
LPDDR 쓰기 명령
```

L1에서 dirty cache line을 내보내더라도 데이터가 즉시 LPDDR에 기록되는 것은 아닙니다. 데이터는 L2 또는 SLC에서 계속 dirty 상태로 남을 수 있습니다. LPDDR에 최종 기록되는 시점은 하위 cache의 교체 동작과 memory controller 정책에 따라 정해집니다.

Write-back은 dirty cache line 교체, 명시적인 cache clean, 다른 CPU의 소유권 요청, 전원 상태 전환, CPU 내부의 선제 처리에서 발생할 수 있습니다. 수정하지 않은 clean cache line은 데이터를 다시 쓰지 않고 제거할 수 있습니다.

<sub><em>Write-back: dirty cache line의 최신 데이터를 하위 cache 또는 system memory 방향으로 기록하는 동작입니다.</em></sub><br>
<sub><em>Dirty line: CPU store 이후 현재 cache가 하위 계층보다 최신 데이터를 보유한 cache line입니다.</em></sub><br>
<sub><em>Eviction: 새로운 line을 배치하기 위해 기존 cache line을 해당 cache level에서 제거하는 동작입니다.</em></sub><br>
<sub><em>Clean eviction: 수정되지 않은 cache line을 data write 없이 해당 cache level에서 제거하는 동작입니다.</em></sub>

## Cache와 DRAM의 데이터가 일치하는 시점

Write-Back memory에서는 dirty cache line이 해당 주소의 최신 데이터를 보유합니다. Write-back이 완료되기 전에는 LPDDR에 이전 값이 남아 있을 수 있습니다. 다른 coherent CPU가 같은 physical address를 읽으면 coherency 회로가 최신 데이터를 보유한 cache를 찾아 데이터를 전달하거나 write-back을 수행합니다.

Coherency가 보장하는 것은 같은 coherency 영역에 속한 CPU와 장치가 규칙에 따라 최신 값을 읽는다는 점입니다. 모든 cache와 LPDDR의 저장 값이 항상 같다는 의미는 아닙니다. 두 값은 해당 cache line의 clean 또는 write-back이 완료된 뒤에 같아집니다.

<sub><em>Coherency: 여러 CPU 또는 coherent device가 동일한 physical address의 최신 데이터를 일관되게 관찰하도록 관리하는 protocol입니다.</em></sub><br>
<sub><em>Coherent observer: 동일 coherency domain에 참여하여 cache 상태와 최신 데이터를 protocol에 따라 조회하는 CPU 또는 device입니다.</em></sub>

## CopyThread의 cache 접근

### 원본 block을 읽을 때

```text
원본 cache line 읽기
 → L1에 있으면 즉시 읽음
 → L1에 없으면 L2·SLC·LPDDR에서 가져옴
 → CPU register에 저장
 → checksum 계산
```

### 대상 block에 쓸 때

```text
대상 cache line에 쓰기
 → 해당 cache line의 쓰기 권한 획득
 → 필요한 경우 cache line을 먼저 가져옴
 → cache line 수정
 → dirty 상태로 변경
 → 이후 하위 cache 또는 LPDDR 방향으로 write-back
```

대상 block의 cache line 전체를 연속으로 덮기 때문에 일부 CPU는 연속 쓰기를 감지하여 불필요한 read-for-ownership을 줄일 수 있습니다. 이 최적화의 적용 여부는 CPU 구현에 따라 달라집니다.

<sub><em>Write allocate: store miss에서 cache line과 write ownership을 확보한 후 cache에서 데이터를 수정하는 정책입니다.</em></sub><br>
<sub><em>Read-for-ownership: store를 수행할 core가 cache line의 최신 데이터와 수정 권한을 확보하기 위해 발생시키는 coherency transaction입니다.</em></sub>

## LPDDR 접근량이 증가하는 원인

여러 Worker가 L1, L2, SLC보다 훨씬 큰 메모리 영역을 동시에 처리하기 때문입니다.

| 계층 | 일반적인 용량 범위 예시 |
|---|---:|
| 테스트 중 반복해서 접근하는 전체 메모리 | 수백 MiB~수 GiB |
| L1D | 수십 KiB |
| L2 | 수백 KiB~수 MiB |
| SLC | 수 MiB 이상, SoC별 상이 |

Worker는 임의로 선택한 1 MiB 원본 block과 대상 block을 계속 바꿉니다. 따라서 이전에 사용한 cache line이 용량 부족이나 cache index 충돌로 교체됩니다. 각 block 내부는 앞에서 뒤로 연속해서 접근하므로 hardware prefetch가 다음 cache line을 미리 요청할 수 있고, 여러 memory request가 동시에 진행될 수 있습니다.

<sub><em>Working set: 일정 시간 동안 workload가 접근하는 전체 데이터 범위입니다.</em></sub><br>
<sub><em>Prefetch: 향후 접근할 가능성이 있는 cache line을 실제 load보다 먼저 요청하는 동작입니다.</em></sub><br>
<sub><em>Outstanding request: 응답 완료 이전에 cache 또는 memory system에 계류 중인 transaction입니다.</em></sub>

그 결과 다음 요청이 증가합니다.

- 원본 cache line이 cache에 없으면 하위 cache 또는 LPDDR 읽기 요청이 발생합니다.
- 대상의 dirty cache line이 교체되면 하위 계층으로 쓰기 요청이 발생합니다.
- 여러 CPU core가 동시에 접근하면 NoC와 DMC에서 대기하는 요청 수가 증가합니다.
- DMC는 대기 중인 요청의 순서를 조정하고 읽기·쓰기를 묶어서 처리합니다.

따라서 CPU 명령의 실행 순서와 LPDDR 명령의 처리 순서는 같지 않을 수 있습니다.

## `-W` 옵션의 ARM64 복사 방식

현재 분석한 AArch64 `AdlerMemcpyAsm()`은 반복 한 번에 64 B를 처리합니다 (`src/adler32memcpy.cc:402`).

> **파일:** `src/adler32memcpy.cc` · **함수:** `AdlerMemcpyAsm()` AArch64 구간 · **기준:** `73b9df2`

```cpp
asm volatile (
    // Preload upcoming cacheline.
    "prfm pldl1strm, [" src_r ", #0 ];\n"
    "prfm pldl1strm, [" src_r ", #64 ];\n"
    "prfm pldl1strm, [" src_r ", #128 ];\n"
    "prfm pldl1strm, [" src_r ", #192];\n"
    "prfm pldl1strm, [" src_r ", #256];\n"

    "TOP:\n"
    "prfm pldl1strm, [" src_r ", #320];\n"
    "prfm pldl1strm, [" src_r ", #384];\n"
    "ld1 {v8.2d, v9.2d, v10.2d, v11.2d}, [" src_r "], #64;\n"
    "st1 {v8.2d, v9.2d, v10.2d, v11.2d}, [" dst_r "], #64;\n"
    // ... checksum vector 연산과 반복 제어가 이어진다.
);
```

**코드 설명:** 반복 한 번에 원본 64 B를 NEON register로 읽고 대상 64 B에 씁니다. `prfm pldl1strm`은 원본 데이터를 L1 cache로 미리 가져오도록 CPU에 전달하는 참고 정보입니다. Cache를 우회하거나 LPDDR 접근을 강제하는 명령은 아닙니다. 실제 LPDDR 접근량은 테스트 메모리 크기, cache 용량, prefetch 구현, 동시에 실행되는 Worker 수에 따라 달라집니다.

핵심 명령은 다음과 같습니다.

```asm
prfm pldl1strm, [src, ...]
ld1  {v8.2d, v9.2d, v10.2d, v11.2d}, [src], #64
st1  {v8.2d, v9.2d, v10.2d, v11.2d}, [dst], #64
add  ... checksum accumulators ...
```

- `prfm pldl1strm`: 원본 데이터를 L1 cache에 미리 가져오도록 요청
- `ld1`: 원본 64 B를 vector register로 읽기
- `st1`: vector register의 64 B를 대상 주소에 쓰기
- vector add: modified-Adler checksum 계산

이 `st1`은 일반 cacheable memory에 대한 보통의 vector 쓰기 명령입니다. AArch64 명령 자체에는 x86 `movntdq`와 같은 non-temporal store 의미가 없으며 cache 우회 속성도 지정하지 않습니다.

<sub><em>Non-temporal store: cache 오염을 줄이기 위해 일반 cache allocation을 최소화하도록 설계된 store 방식입니다.</em></sub>

## `-F` 옵션의 복사 방식

`-F`를 지정하면 `CrcCopyPage()`를 사용하지 않고 C library의 `memcpy()`로 block을 복사합니다.

장점:

- checksum 계산량이 줄어듭니다.
- bionic이 제공하는 SoC·복사 크기별 최적화를 사용할 수 있습니다.
- 메모리 복사 속도가 증가할 수 있습니다.

단점:

- 원본 데이터 오류는 해당 block을 다시 원본으로 사용하거나 마지막 전체 검사를 수행할 때 발견될 수 있습니다.
- 대상 데이터도 해당 block을 다시 읽거나 마지막 전체 검사를 수행할 때까지 검사가 지연될 수 있습니다.
- 실제 복사 명령은 Android, bionic, SoC, library version에 따라 달라집니다.

## `-i` 옵션의 데이터 반전과 cache 처리

`InvertThread`는 32-bit 값을 읽고 모든 bit를 반전한 뒤 같은 위치에 다시 씁니다. 이 동작은 read-modify-write이며 64 B마다 `FastFlushHint()`를 호출합니다.

AArch64의 `FastFlush()`는 다음 순서로 실행됩니다 (`src/os.h:171`).

```asm
dc cvau, address
dsb ish
ic ivau, address
dsb ish
isb
```

각 명령의 의미는 다음과 같습니다.

- `dc cvau`: data cache를 Point of Unification 방향으로 clean합니다.
- `ic ivau`: instruction cache의 해당 line을 무효화합니다.
- 명령 순서는 프로그램 코드를 실행 중에 변경할 때 사용하는 data·instruction cache 동기화 과정과 유사합니다.
- Data cache clean의 완료 지점은 PoU입니다.
- `ic ivau`는 data cache가 아니라 instruction cache에 적용됩니다.

`-i`는 데이터 반전, 접근 방향 전환, PoU까지의 cache 관리 명령, barrier 실행으로 구성됩니다. LPDDR 요청은 이러한 동작이 cache 계층을 거치는 과정에서 필요할 때 발생합니다.

<sub><em>PoU: Point of Unification의 약어이며 instruction fetch와 data access가 동일한 데이터 복사본을 관찰하도록 하는 계층 지점입니다.</em></sub><br>
<sub><em>PoC: Point of Coherency의 약어이며 coherency 참여자가 동일한 데이터 복사본을 관찰하도록 하는 계층 지점입니다.</em></sub><br>
<sub><em>RMW: Read-Modify-Write의 약어이며 기존 값을 읽고 연산한 결과를 동일 위치에 다시 쓰는 동작입니다.</em></sub>

## ARM64에서 오류 데이터를 다시 읽을 때의 제한

오류 처리 코드는 기대값과 실제값이 다르면 cache line을 정리한 뒤 같은 주소를 다시 읽어, 일시적인 읽기 오류와 저장된 데이터 오류를 구분하려고 합니다.

공통 AArch64 기능 확인 코드는 `has_vector_ = true`를 설정하지만 `has_clflush_`는 false로 유지합니다. `OsLayer::Flush()`는 `has_clflush_`가 true일 때만 `FastFlush()`를 호출합니다 (`src/os.cc:263`).

따라서 공통 ARM64 build에서는 오류 데이터를 다시 읽기 전에 `Flush()`가 cache 관리 명령을 실행하지 않을 수 있습니다. Checksum 불일치 자체는 유효한 오류 결과로 기록하되, 첫 번째 값과 다시 읽은 값의 차이로 읽기 오류와 쓰기 오류를 분류할 때에는 이 구현 조건을 함께 고려해야 합니다.

## CPU cache와 파일 page cache 구분

CPU cache와 Linux의 파일 page cache는 서로 다른 계층입니다.

| 항목 | 위치/관리자 | `O_DIRECT` 영향 |
|---|---|---|
| L1/L2/SLC | CPU와 SoC hardware | 일반 cacheable·coherent 경로를 계속 사용 |
| filesystem page cache | Linux kernel이 사용하는 RAM | 가능한 경우 우회 |
| UFS·저장 장치 내부 cache | 저장 장치 controller | 장치 자체 정책에 따라 동작 |

`FileThread`의 `O_DIRECT`는 filesystem page cache를 가능한 범위에서 우회하도록 요청합니다. DMA buffer coherency와 SoC 내부 데이터 이동은 기기의 I/O 구성에 따라 계속 발생합니다.

<sub><em>Page cache: Linux kernel이 file 데이터를 RAM에 보관하여 file I/O를 처리하는 cache 계층입니다.</em></sub><br>
<sub><em>O_DIRECT: filesystem page cache 사용을 최소화하도록 kernel에 요청하는 file open flag입니다.</em></sub>

## 실제 LPDDR 접근량 확인 방법

다음 값은 측정하는 위치가 다르므로 서로 같지 않을 수 있습니다.

```text
stressapptest가 처리했다고 계산한 byte
≠ CPU가 실행한 읽기·쓰기 명령의 byte
≠ cache refill·write-back byte
≠ SLC·NoC를 통과한 byte
≠ DMC가 처리한 byte
≠ LPDDR 읽기·쓰기 명령 수 × burst 크기
```

LPDDR 접근량은 DMC의 읽기·쓰기 byte 또는 명령 counter로 확인해야 합니다. CPU cache miss counter만 사용할 경우에는 prefetch, shared cache hit, write allocate, write-back, snoop 요청을 별도로 고려해야 합니다.

<sub><em>DMC counter: DRAM memory controller에서 처리한 read/write byte, command, queue 또는 bank event를 집계하는 hardware counter입니다.</em></sub><br>
<sub><em>Snoop traffic: 다른 cache의 data 또는 ownership 상태를 조회·변경하기 위해 발생하는 coherency transaction입니다.</em></sub>

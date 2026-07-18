# ARM64 cache policy와 실제 LPDDR traffic

## stressapptest가 cache policy를 설정하는가?

stressapptest는 테스트 memory의 page-table cache attribute를 변경하지 않는다. cache 관련 동작은 다음 세 계층에서 결정된다.

1. **Memory attribute policy**: kernel page table/MAIR가 Normal, Cacheable, Write-Back, Shareable 등을 정한다.
2. **Microarchitecture policy**: CPU가 replacement, prefetch, write allocation, streaming 최적화, write-back 시점을 정한다.
3. **Workload access policy**: stressapptest가 block 크기, 주소 선택, read/write 순서와 worker 수를 정한다.

stressapptest가 직접 제어하는 것은 주로 3번이다.

<sub><em>Memory attribute policy: kernel이 page table과 MAIR를 통해 memory type, cacheability 및 shareability를 지정하는 정책입니다.</em></sub><br>
<sub><em>Microarchitecture policy: CPU 구현이 replacement, prefetch, write allocation 및 write-back 시점을 결정하는 내부 정책입니다.</em></sub><br>
<sub><em>Workload access policy: software가 주소, 크기, 순서, read/write 비율 및 thread 수를 구성하는 방식입니다.</em></sub>

## 소스 코드로 확인하는 ARM64 cache maintenance

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

**해석:** `dc cvau`는 해당 virtual address의 data cache line을 Point of Unification 방향으로 clean합니다. `ic ivau`는 같은 주소의 instruction cache line을 invalidate합니다. 이 조합은 data cache line을 Point of Coherency까지 clean-and-invalidate하는 `dc civac`와 다릅니다. 따라서 ARM64의 `FastFlush()`를 “DRAM까지 강제 write하고 data cache를 완전히 비우는 명령”으로 해석할 수 없습니다.

<sub><em>Point of Unification, PoU: instruction fetch와 data access가 동일한 memory copy를 관찰하도록 합류하는 cache hierarchy 지점입니다.</em></sub><br>
<sub><em>Point of Coherency, PoC: 해당 shareability domain의 CPU와 coherent observer가 동일한 memory copy를 관찰하는 지점입니다.</em></sub><br>
<sub><em>Cache clean: dirty data를 지정된 cache hierarchy 지점 방향으로 기록하고 line의 valid 상태는 유지할 수 있는 동작입니다.</em></sub><br>
<sub><em>Cache invalidate: cache line의 valid 상태를 제거하여 이후 access가 다시 line을 획득하게 하는 동작입니다.</em></sub>

## 일반적인 cached write 흐름

```text
CPU store
  ↓
store buffer
  ↓
L1D line 수정, dirty
  ↓ eviction/clean/coherency action
L2 또는 cluster cache
  ↓
SLC/LLCC
  ↓
DMC write queue
  ↓
LPDDR write command
```

L1 dirty eviction의 write-back 대상은 다음 coherency/cache 계층일 수 있다. 해당 line은 L2 또는 SLC에서 dirty 상태로 유지될 수 있으며, 최종 LPDDR write 시점은 하위 계층의 replacement와 memory controller 정책에 따라 결정된다.

Write-back은 dirty eviction, explicit clean, coherent ownership request, power-state 처리 또는 내부 선제 write-back에서 발생할 수 있다. clean eviction은 data write 없이 line state를 제거한다.

<sub><em>Write-back: dirty cache line의 최신 데이터를 하위 cache 또는 system memory 방향으로 기록하는 동작입니다.</em></sub><br>
<sub><em>Dirty line: CPU store 이후 현재 cache가 하위 계층보다 최신 데이터를 보유한 cache line입니다.</em></sub><br>
<sub><em>Eviction: 새로운 line을 배치하기 위해 기존 cache line을 해당 cache level에서 제거하는 동작입니다.</em></sub><br>
<sub><em>Clean eviction: 수정되지 않은 cache line을 data write 없이 해당 cache level에서 제거하는 동작입니다.</em></sub>

## Write-Back 상태의 cache와 DRAM 데이터 관계

Write-Back memory에서는 dirty cache line이 해당 address의 최신 데이터를 보유한다. LPDDR에는 write-back 이전 값이 남아 있을 수 있다. 다른 coherent core가 해당 physical address를 읽으면 coherency fabric이 dirty owner를 확인하여 최신 데이터를 전달하거나 write-back을 수행한다.

coherency가 제공하는 보장은 coherent observer가 protocol 규칙에 따라 최신 데이터를 관찰하는 것이다. cache와 LPDDR의 저장 값이 일치하는 시점은 clean/write-back 완료 조건에 따라 결정된다.

<sub><em>Coherency: 여러 CPU 또는 coherent device가 동일한 physical address의 최신 데이터를 일관되게 관찰하도록 관리하는 protocol입니다.</em></sub><br>
<sub><em>Coherent observer: 동일 coherency domain에 참여하여 cache 상태와 최신 데이터를 protocol에 따라 조회하는 CPU 또는 device입니다.</em></sub>

## 기본 CopyThread의 cache 동작

### Source

```text
load source line
 → L1 hit 또는 miss
 → miss면 L2/SLC/LPDDR에서 line fill
 → register로 load
 → checksum 누산
```

### Destination

```text
store destination line
 → ownership 획득
 → 일반적으로 write allocate/line fill 가능
 → cache line 수정
 → dirty 상태
 → 나중에 아래 계층으로 write-back
```

destination 전체 cache line을 연속으로 덮으므로 일부 CPU는 full-line streaming을 감지해 불필요한 read-for-ownership을 줄일 수 있다. 이는 implementation-specific다.

<sub><em>Write allocate: store miss에서 cache line과 write ownership을 확보한 후 cache에서 데이터를 수정하는 정책입니다.</em></sub><br>
<sub><em>Read-for-ownership: store를 수행할 core가 cache line의 최신 데이터와 수정 권한을 확보하기 위해 발생시키는 coherency transaction입니다.</em></sub>

## Cacheable memory에서 LPDDR traffic이 발생하는 조건

L1/L2/SLC보다 훨씬 큰 memory를 여러 worker가 동시에 사용하기 때문이다.

| 계층 | 일반적인 용량 범위 예시 |
|---|---:|
| 테스트 working set | 수백 MiB~수 GiB |
| L1D | 수십 KiB |
| L2 | 수백 KiB~수 MiB |
| SLC | 수 MiB 이상, SoC별 상이 |

worker는 random 1 MiB source/destination을 계속 변경하므로 이전 line이 capacity/conflict replacement 대상이 된다. block 내부 순차 access는 hardware prefetch가 인식할 수 있는 연속 주소열을 제공하고 여러 outstanding request 생성을 허용한다.

<sub><em>Working set: 일정 시간 동안 workload가 접근하는 전체 데이터 범위입니다.</em></sub><br>
<sub><em>Prefetch: 향후 접근할 가능성이 있는 cache line을 실제 load보다 먼저 요청하는 동작입니다.</em></sub><br>
<sub><em>Outstanding request: 응답 완료 이전에 cache 또는 memory system에 계류 중인 transaction입니다.</em></sub>

결과적으로:

- source miss가 refill/read traffic을 만듦
- destination dirty eviction이 write-back traffic을 만듦
- 여러 core가 NoC와 DMC queue depth를 증가시킴
- DMC가 request를 reorder하고 read/write batch를 구성함

CPU instruction 순서와 LPDDR command 순서는 동일하지 않을 수 있다.

## ARM64 `-W` 경로

현재 GitHub master의 AArch64 `AdlerMemcpyAsm()`은 한 loop에서 64 B를 처리한다 (`src/adler32memcpy.cc:402`).

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
    // ... checksum vector 연산과 loop 제어가 이어진다.
);
```

**해석:** 한 iteration에서 source 64 B를 NEON register로 load하고 destination 64 B를 store합니다. `prfm pldl1strm`은 L1 streaming preload hint이며 cache bypass 또는 DRAM access 보장 명령이 아닙니다. working set, cache capacity, prefetch 구현 및 concurrent worker에 따라 실제 LPDDR traffic이 결정됩니다.

핵심 instruction은 다음과 같다.

```asm
prfm pldl1strm, [src, ...]
ld1  {v8.2d, v9.2d, v10.2d, v11.2d}, [src], #64
st1  {v8.2d, v9.2d, v10.2d, v11.2d}, [dst], #64
add  ... checksum accumulators ...
```

- `prfm pldl1strm`: source를 L1 streaming hint로 prefetch
- `ld1`: 64 B vector load
- `st1`: 64 B vector store
- vector add: modified-Adler checksum 계산

이 `st1`은 normal cacheable memory에 대한 일반 vector store로 실행된다. AArch64 instruction 자체에는 x86 `movntdq`의 non-temporal store 의미가 포함되지 않으며 cache bypass 속성도 지정되지 않는다.

<sub><em>Non-temporal store: cache 오염을 줄이기 위해 일반 cache allocation을 최소화하도록 설계된 store 방식입니다.</em></sub>

## `-F` 경로

`-F`에서는 `CrcCopyPage()` 분기를 비활성화하고 libc `memcpy()` 분기를 실행한다.

장점:

- checksum arithmetic 감소
- bionic의 SoC/size 최적화 경로 사용 가능
- memory throughput 증가 가능

단점:

- source corruption 검출 시점이 이후 source 선택 또는 final check로 이동
- destination은 나중에 source가 되거나 final check까지 검증이 지연될 수 있음
- 실제 instruction은 Android/bionic/version/SoC에 따라 달라짐

## `-i`와 cache maintenance

InvertThread는 32-bit word를 읽어 `~value`를 다시 쓰므로 read-modify-write다. 64 B마다 `FastFlushHint()`를 호출한다.

AArch64의 `FastFlush()`는 다음 sequence다 (`src/os.h:171`).

```asm
dc cvau, address
dsb ish
ic ivau, address
dsb ish
isb
```

중요한 해석:

- `dc cvau`: D-cache를 Point of Unification 방향으로 clean
- `ic ivau`: instruction cache invalidate
- self-modifying code용 D/I cache 일치 sequence와 유사
- clean 완료 지점은 PoU로 지정됨
- `ic ivau`의 대상은 instruction cache임

`-i` mode의 동작은 Invert RMW, 접근 방향 전환, PoU cache maintenance 및 barrier 실행으로 구성된다. LPDDR transaction은 이 동작이 cache hierarchy를 통과한 결과로 발생한다.

<sub><em>PoU: Point of Unification의 약어이며 instruction fetch와 data access가 동일한 데이터 복사본을 관찰하도록 하는 계층 지점입니다.</em></sub><br>
<sub><em>PoC: Point of Coherency의 약어이며 coherency 참여자가 동일한 데이터 복사본을 관찰하도록 하는 계층 지점입니다.</em></sub><br>
<sub><em>RMW: Read-Modify-Write의 약어이며 기존 값을 읽고 연산한 결과를 동일 위치에 다시 쓰는 동작입니다.</em></sub>

## 오류 재검사의 `Flush()` ARM64 한계

오류 처리 코드는 mismatch 후 cache line을 flush하고 reread하여 read error와 write/storage error를 구분하려 한다.

generic AArch64 feature detection은 `has_vector_ = true`를 설정하며 `has_clflush_`는 false 상태를 유지한다. `OsLayer::Flush()`는 `has_clflush_`가 true일 때만 `FastFlush()`를 호출한다 (`src/os.cc:263`).

generic ARM64 build에서는 mismatch reread 전 `Flush()`가 cache maintenance를 실행하지 않을 수 있다. mismatch 검출 결과는 checksum 비교 결과로 사용하며, actual/reread 차이에 기반한 read/write 오류 분류에는 이 구현 조건을 함께 기록한다.

## CPU cache와 filesystem page cache

두 cache는 다르다.

| 항목 | 위치/관리자 | `O_DIRECT` 영향 |
|---|---|---|
| L1/L2/SLC | CPU/SoC hardware | 일반 cacheable/coherent 경로 유지 |
| filesystem page cache | Linux kernel RAM | 가능한 경우 우회 |
| UFS/device internal cache | storage controller/device | 별도 정책 |

FileThread의 `O_DIRECT`는 filesystem page cache 동작 범위에 적용된다. DMA buffer coherency와 system interconnect traffic은 platform I/O 경로에 따라 발생한다.

<sub><em>Page cache: Linux kernel이 file 데이터를 RAM에 보관하여 file I/O를 처리하는 cache 계층입니다.</em></sub><br>
<sub><em>O_DIRECT: filesystem page cache 사용을 최소화하도록 kernel에 요청하는 file open flag입니다.</em></sub>

## 실제 LPDDR traffic 판정

다음 값은 서로 같지 않을 수 있다.

```text
SAT logical bytes
≠ CPU issued load/store bytes
≠ cache refill/write-back bytes
≠ SLC/NoC bytes
≠ DMC data bytes
≠ LPDDR read/write command 수 × burst 크기
```

LPDDR 접근량은 DMC read/write byte 또는 command counter로 확인한다. CPU cache miss counter를 사용할 때에는 prefetch, shared cache hit, write allocate, write-back 및 snoop traffic을 별도 항목으로 고려한다.

<sub><em>DMC counter: DRAM memory controller에서 처리한 read/write byte, command, queue 또는 bank event를 집계하는 hardware counter입니다.</em></sub><br>
<sub><em>Snoop traffic: 다른 cache의 data 또는 ownership 상태를 조회·변경하기 위해 발생하는 coherency transaction입니다.</em></sub>

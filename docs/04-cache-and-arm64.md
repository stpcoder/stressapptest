# ARM64 cache policy와 실제 LPDDR traffic

## stressapptest가 cache policy를 설정하는가?

아니다. 세 층을 분리해야 한다.

1. **Memory attribute policy**: kernel page table/MAIR가 Normal, Cacheable, Write-Back, Shareable 등을 정한다.
2. **Microarchitecture policy**: CPU가 replacement, prefetch, write allocation, streaming 최적화, write-back 시점을 정한다.
3. **Workload access policy**: stressapptest가 block 크기, 주소 선택, read/write 순서와 worker 수를 정한다.

stressapptest가 직접 제어하는 것은 주로 3번이다.

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

L1 dirty eviction은 곧바로 LPDDR write라는 뜻이 아니다. 데이터가 L2/SLC에서 다시 dirty 상태로 머물 수 있다.

Write-back은 dirty eviction 외에도 explicit clean, coherent ownership request, power-state 처리, 내부 선제 write-back 등으로 발생할 수 있다. 반대로 clean eviction은 data write 없이 line을 버릴 수 있다.

## cache와 DRAM 값이 달라도 되는 이유

Write-Back memory에서는 dirty cache line이 최신 authoritative copy이고 LPDDR가 이전 값을 가지고 있어도 정상이다. 같은 physical address를 다른 coherent core가 읽으면 coherency fabric이 dirty owner를 찾아 최신값을 forward하거나 write-back시킨다.

즉 보장 대상은 “모든 coherent observer가 올바른 최신값을 본다”는 것이지 “LPDDR cell이 매 순간 cache와 동일하다”는 것이 아니다.

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

## 왜 cache를 disable하지 않아도 DRAM에 도달하는가?

L1/L2/SLC보다 훨씬 큰 memory를 여러 worker가 동시에 사용하기 때문이다.

```text
예: 4 GiB working set
vs 수십 KiB L1D
vs 수백 KiB~수 MiB L2
vs 수 MiB급 SLC
```

worker는 random 1 MiB source/destination을 계속 바꾸므로 오래된 line이 capacity/conflict replacement 대상이 된다. block 내부의 순차 access는 prefetch와 여러 outstanding request를 활성화하기 쉽다.

결과적으로:

- source miss가 refill/read traffic을 만듦
- destination dirty eviction이 write-back traffic을 만듦
- 여러 core가 NoC와 DMC queue depth를 증가시킴
- DMC가 request를 reorder하고 read/write batch를 구성함

CPU instruction 순서와 LPDDR command 순서는 동일하지 않을 수 있다.

## ARM64 `-W` 경로

현재 GitHub master의 AArch64 `AdlerMemcpyAsm()`은 한 loop에서 64 B를 처리한다 (`src/adler32memcpy.cc:402`).

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

이 `st1`은 일반 cached store다. x86 구현의 `movntdq` non-temporal store와 동일하지 않고, AArch64에서 cache bypass를 보장하지 않는다.

## `-F` 경로

`-F`는 transaction마다 checksum을 계산하는 `CrcCopyPage()` 대신 libc `memcpy()`를 사용한다.

장점:

- checksum arithmetic 감소
- bionic의 SoC/size 최적화 경로 사용 가능
- memory throughput 증가 가능

단점:

- source corruption을 copy 시점에 즉시 확인하지 않음
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
- `dc cvac`처럼 PoC까지 clean한다고 명시된 동작이 아님
- `ic ivau`는 data cache invalidate가 아님

따라서 `-i`를 “LPDDR direct read/write mode”로 설명하면 틀리다. Invert의 RMW, 방향 전환, cache maintenance 및 barrier overhead가 결합된 mode다.

## 오류 재검사의 `Flush()` ARM64 한계

오류 처리 코드는 mismatch 후 cache line을 flush하고 reread하여 read error와 write/storage error를 구분하려 한다.

그러나 generic AArch64 feature detection은 `has_vector_ = true`만 설정하고 `has_clflush_`는 설정하지 않는다. `OsLayer::Flush()`는 `has_clflush_`가 true일 때만 `FastFlush()`를 호출한다 (`src/os.cc:263`).

따라서 generic ARM64 build에서 mismatch reread 전 `Flush()`는 사실상 no-op일 수 있다. mismatch 검출 자체는 유효하지만 read/write 오류 분류는 제한적으로 해석해야 한다.

## CPU cache와 filesystem page cache

두 cache는 다르다.

| 항목 | 위치/관리자 | `O_DIRECT` 영향 |
|---|---|---|
| L1/L2/SLC | CPU/SoC hardware | 우회하지 않음 |
| filesystem page cache | Linux kernel RAM | 가능한 경우 우회 |
| UFS/device internal cache | storage controller/device | 별도 정책 |

FileThread가 `O_DIRECT`를 사용해도 DMA buffer의 CPU cache coherency와 system interconnect traffic은 계속 존재한다.

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

실제 LPDDR 접근 여부는 DMC read/write byte 또는 command counter가 가장 직접적이다. CPU cache miss counter만으로 DRAM byte를 환산하면 prefetch, shared cache hit, write allocate, writeback, snoop traffic을 놓칠 수 있다.

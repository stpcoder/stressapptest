# 초보자를 위한 용어집

## 용어와 실제 코드의 연결

> **파일:** `src/sattypes.h`, `src/finelock_queue.h` · **구간:** 기본 단위와 block 상태 predicate · **기준:** `73b9df2`

```cpp
static const int kSatPageSize = (1024LL*1024LL);  // SAT block
static const int kCacheLineSize = 64;             // 처리 간격 상수

static bool page_is_valid(struct page_entry *pe) {
  return pe->pattern != NULL;
}
static bool page_is_empty(struct page_entry *pe) {
  return pe->pattern == NULL;
}
```

**해석:** 이 매뉴얼의 `SAT block`, `cache line`, `valid`, `empty`는 위 구현에 직접 대응합니다. Linux page, physical address, LPDDR row는 별도의 OS·hardware 단위이며 `kSatPageSize`와 동일한 개념이 아닙니다.

## Address와 memory

### Virtual address, VA

프로세스가 pointer로 사용하는 주소. MMU가 physical address로 변환한다.

### Physical address, PA

<sub><em>Physical address, PA: CPU/NoC가 system memory 또는 MMIO를 식별하는 주소입니다. LPDDR row/column 좌표는 DMC address mapping을 추가로 적용하여 계산합니다.</em></sub>

### PFN

Physical Frame Number. physical page의 번호다.

```text
PA = PFN × PAGE_SIZE + page offset
```

### IOVA

DMA device가 사용하는 I/O virtual address. SMMU/IOMMU가 system PA로 변환할 수 있다.

### MMU

Memory Management Unit. virtual address translation과 access permission/memory attribute를 적용한다.

### TLB

Translation Lookaside Buffer. 최근 VA→PA page-table translation을 cache한다.

### First touch

anonymous virtual page를 처음 실제로 read/write하여 kernel이 physical backing page를 할당하게 만드는 access.

## stressapptest 단위

### SAT block

queue가 관리하는 논리적 memory chunk. 소스에서는 page라고 부르며 기본 1 MiB다.

### Linux page

kernel/MMU가 mapping하는 단위. AArch64에서 4/16/64 KiB 등 구성이 가능하다.

### Cache line

<sub><em>Cache line: cache fill, coherency 및 write-back이 처리하는 기본 data 단위입니다. mobile ARM에서는 일반적으로 64 B이며 target implementation에서 최종 확인합니다.</em></sub>

### Working set

일정 시간 동안 workload가 실제 접근하는 data 전체 크기. cache보다 크면 capacity miss가 증가한다.

## Cache

### Cache hit

요청 data가 현재 cache에 있어 아래 memory 계층까지 갈 필요가 없는 경우.

### Cache miss/refill

data가 없어 L2/SLC/DRAM 등 아래 계층에서 cache line을 가져오는 동작.

### Write-Back

<sub><em>Write-Back: CPU store가 cache line을 수정하고 dirty 상태로 표시한 후, eviction 또는 clean 조건에서 하위 cache나 system memory 방향으로 기록하는 정책입니다.</em></sub>

### Write-Through

store 데이터를 cache와 하위 memory 계층 방향으로 함께 전달하는 정책. write buffer와 하위 cache가 포함될 수 있으며 LPDDR command 시점은 memory hierarchy 상태에 따라 결정된다.

### Dirty line

이 cache가 아래 계층보다 최신 data를 가진 line. 버리기 전에 write-back해야 한다.

### Clean line

해당 cache level 관점에서 하위 coherency point보다 최신 수정본을 보유하지 않은 line. 다른 cache level의 dirty 상태는 독립적으로 존재할 수 있다.

### Eviction

<sub><em>Eviction: 새로운 line을 배치하기 위해 기존 victim line을 해당 cache level에서 제거하는 동작입니다. dirty victim은 제거 전에 write-back이 필요합니다.</em></sub>

### Write allocate

<sub><em>Write allocate: store miss에서 cache line과 write ownership을 확보한 후 cache에서 데이터를 수정하는 정책입니다.</em></sub>

### RFO/ownership request

다른 shared copy를 무효화하고 이 core가 line을 수정할 권한을 얻기 위한 transaction을 설명하는 일반적 표현.

### Prefetch

future access를 예상해 CPU가 instruction보다 먼저 line을 가져오는 기능. `prfm`은 software hint이고 hardware prefetcher도 별도로 동작할 수 있다.

### Coherency

여러 CPU/device가 같은 physical address의 최신값을 일관되게 보도록 하는 규칙과 protocol.

### Memory ordering/consistency

서로 다른 주소의 load/store가 다른 observer에게 어떤 순서로 보이는지 정하는 규칙. 같은 주소 coherency와 별개이며 barrier/atomic이 필요할 수 있다.

### Snoop

다른 cache가 특정 physical address의 data 또는 ownership을 갖는지 묻고 state를 변경하는 coherency transaction.

### PoC

Point of Coherency. relevant observer가 같은 memory copy를 보도록 보장되는 memory hierarchy 지점.

### PoU

Point of Unification. instruction fetch와 data access가 같은 copy를 보도록 보장되는 지점. `dc cvau`의 U가 PoU를 뜻한다.

### DMB/DSB/ISB

- DMB: memory access ordering barrier
- DSB: 앞선 operation completion까지 기다리는 stronger barrier
- ISB: instruction pipeline/context synchronization

barrier는 architecture가 정의한 ordering 또는 completion 조건을 적용한다. cache clean 범위는 함께 실행한 cache maintenance instruction의 PoU/PoC 조건으로 결정된다.

## SoC/LPDDR

### SLC/LLCC

System/Last-Level Cache. 여러 CPU cluster 또는 device가 공유할 수 있는 on-chip cache.

### NoC

Network-on-Chip. CPU, GPU, NPU, UFS, DMC 등을 연결하는 on-chip interconnect.

### DMC

<sub><em>DMC: DRAM Memory Controller의 약어이며 system request를 LPDDR command로 schedule하고 channel, bank, row 및 column을 선택합니다.</em></sub>

### Channel

독립적으로 transaction을 처리할 수 있는 memory interface 경로. PA interleaving 방식은 SoC마다 다르다.

### Rank

같은 channel command/address를 공유하면서 chip-select 등으로 구분되는 DRAM device 집합.

### Bank/Bank Group

DRAM 내부에서 어느 정도 독립적으로 activate/read/write할 수 있는 배열 구획.

### Row/Column

Bank 내부 cell address. Row를 activate해 row buffer에 연 뒤 column read/write를 수행한다.

### Row hit/miss

- row hit: 원하는 row가 이미 active
- row miss/conflict: 다른 row를 precharge/activate해야 함

SAT의 random 선택 단위는 1 MiB virtual block이다. 실제 row는 VA→PA translation과 DMC address mapping으로 결정된다.

### DQ/CA

- DQ: DRAM data signal
- CA: command/address signal

SAT pattern word와 physical DQ pin mapping 사이에는 cache/DMC swizzle/interleave 등 여러 단계가 있다.

## Worker와 검증

### Worker

특정 부하 loop를 수행하는 pthread. Copy, Check, Invert, File, Network, Disk, CPU 등이 있다.

### Valid block

expected Pattern metadata가 있고 source/check 대상으로 사용할 수 있는 SAT block.

### Empty block

destination write 대상으로 사용할 수 있는 SAT block. allocation과 physical backing은 유지되고 pattern metadata가 null로 설정된다.

### Strict mode

기본 mode. copy/I/O 과정에서 checksum으로 data를 지속 검증한다.

### Modified Adler checksum

SAT의 `Crc*` 함수가 사용하는 네 누산기 기반 고속 checksum. CRC polynomial 및 cryptographic hash algorithm은 사용하지 않는다.

### Slow compare

checksum mismatch 후 64-bit word마다 actual/expected를 다시 비교해 상세 오류 위치를 찾는 경로.

### Tag mode

각 cache line 첫 word에 address-derived tag를 저장해 wrong-address data를 찾는 mode.

### RMW

Read-Modify-Write. 기존값을 읽고 계산한 뒤 같은 위치에 다시 쓰는 access. InvertThread의 `x = ~x`가 예다.

## OS와 측정

### Anonymous mmap

file backing 없이 process virtual memory를 확보하는 Linux API. stressapptest 일반 memory allocation 경로다.

### Page cache

Linux kernel이 file contents를 RAM에 보관하는 기능. CPU L1/L2 cache와 독립된 software-managed file cache 계층이다.

### `O_DIRECT`

filesystem page cache 사용을 최소화하도록 kernel에 요청하는 file I/O flag. DMA coherency, CPU cache, LLCC 및 DRAM controller는 platform I/O 경로에 따라 동작한다.

### PMU

Performance Monitoring Unit. CPU/cache/NoC/DMC의 event를 count하는 hardware block.

### Logical bandwidth

<sub><em>Logical bandwidth: software가 처리 block 수와 block 크기로 계산한 byte/s입니다. 실제 cache, NoC, DMC 및 LPDDR traffic은 각 계층 counter로 측정합니다.</em></sub>

### LMKD

Android Low Memory Killer Daemon. memory pressure가 높을 때 process를 kill할 수 있다. SAT process death와 hardware error를 구분해야 한다.

### pstore/ramoops

reboot/panic 전 kernel log를 reserved persistent RAM 등에 남기는 mechanism. 정상 log가 끊긴 failure 분석에 유용하다.

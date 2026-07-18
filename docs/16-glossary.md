# 초보자를 위한 용어집

## Address와 memory

### Virtual address, VA

프로세스가 pointer로 사용하는 주소. MMU가 physical address로 변환한다.

### Physical address, PA

CPU/NoC가 system memory 또는 MMIO를 식별하는 주소. LPDDR row/column 좌표와 동일하지 않다.

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

cache fill, coherency, writeback의 기본 data 단위. mobile ARM에서 흔히 64 B지만 target별 확인이 필요하다.

### Working set

일정 시간 동안 workload가 실제 접근하는 data 전체 크기. cache보다 크면 capacity miss가 증가한다.

## Cache

### Cache hit

요청 data가 현재 cache에 있어 아래 memory 계층까지 갈 필요가 없는 경우.

### Cache miss/refill

data가 없어 L2/SLC/DRAM 등 아래 계층에서 cache line을 가져오는 동작.

### Write-Back

CPU store가 cache line만 먼저 수정하고 dirty로 표시한 뒤 나중에 아래 계층으로 쓰는 정책.

### Write-Through

store를 cache와 그 아래 계층으로 전달하는 정책. write buffer 때문에 “즉시 LPDDR pin 도달”과 완전히 같은 뜻은 아니다.

### Dirty line

이 cache가 아래 계층보다 최신 data를 가진 line. 버리기 전에 write-back해야 한다.

### Clean line

해당 cache level 관점에서 아래 coherency point보다 최신 수정본을 갖지 않은 line. 외부 cache가 dirty일 수 있으므로 L1 clean이 LPDDR 최신을 뜻하지는 않는다.

### Eviction

새 line을 넣기 위해 기존 victim line을 cache에서 제거하는 것. clean이면 버릴 수 있고 dirty면 write-back이 필요하다.

### Write allocate

store miss 때 해당 cache line을 먼저 가져오고 ownership을 얻은 후 cache에서 수정하는 정책.

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

barrier 자체가 모든 cache를 LPDDR까지 flush하는 것은 아니다.

## SoC/LPDDR

### SLC/LLCC

System/Last-Level Cache. 여러 CPU cluster 또는 device가 공유할 수 있는 on-chip cache.

### NoC

Network-on-Chip. CPU, GPU, NPU, UFS, DMC 등을 연결하는 on-chip interconnect.

### DMC

DRAM Memory Controller. system request를 LPDDR command로 scheduling하고 channel/bank/row/column을 선택한다.

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

SAT의 random virtual 1 MiB block 선택이 곧 random row 선택이라는 뜻은 아니다.

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

destination으로 덮어써도 되는 SAT block. OS free memory라는 뜻이 아니다.

### Strict mode

기본 mode. copy/I/O 과정에서 checksum으로 data를 지속 검증한다.

### Modified Adler checksum

SAT가 CRC라는 이름으로 부르는 네 누산기 기반 고속 checksum. 표준 CRC나 cryptographic hash가 아니다.

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

Linux가 file contents를 RAM에 cache하는 기능. CPU L1/L2 cache와 다르다.

### `O_DIRECT`

가능한 경우 filesystem page cache를 우회하는 file I/O flag. CPU cache/LLCC/DRAM controller를 우회하지 않는다.

### PMU

Performance Monitoring Unit. CPU/cache/NoC/DMC의 event를 count하는 hardware block.

### Logical bandwidth

software가 처리했다고 계산한 byte/s. 실제 cache/NoC/DMC/LPDDR traffic과 다를 수 있다.

### LMKD

Android Low Memory Killer Daemon. memory pressure가 높을 때 process를 kill할 수 있다. SAT process death와 hardware error를 구분해야 한다.

### pstore/ramoops

reboot/panic 전 kernel log를 reserved persistent RAM 등에 남기는 mechanism. 정상 log가 끊긴 failure 분석에 유용하다.

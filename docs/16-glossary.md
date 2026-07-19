# 용어 설명

처음 보는 용어는 이 장에서 간단히 확인할 수 있습니다. 각 설명은 이 문서와 stressapptest 소스 코드에서 사용하는 의미를 기준으로 합니다.

## 용어 설명을 읽는 방법

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

**코드 설명:** 이 설명서에서 사용하는 `SAT block`, `cache line`, `valid`, `empty`는 위 코드에 직접 대응합니다. Linux page, physical address, LPDDR row는 운영체제와 hardware가 사용하는 별도 단위이며 `kSatPageSize`와 같은 개념이 아닙니다.

## 주소와 메모리

### Virtual address, VA

프로그램의 pointer에 저장되는 주소입니다. MMU가 physical address로 변환합니다.

### Physical address, PA

<sub><em>Physical address, PA: CPU/NoC가 system memory 또는 MMIO를 식별하는 주소입니다. LPDDR row/column 좌표는 DMC address mapping을 추가로 적용하여 계산합니다.</em></sub>

### PFN

Physical Frame Number의 약어이며 physical page의 번호입니다.

```text
PA = PFN × PAGE_SIZE + page offset
```

### IOVA

DMA 장치가 사용하는 I/O virtual address입니다. SMMU 또는 IOMMU가 system physical address로 변환할 수 있습니다.

### MMU

Memory Management Unit의 약어입니다. Virtual address를 physical address로 변환하고 접근 권한과 메모리 속성을 적용합니다.

### TLB

Translation Lookaside Buffer의 약어입니다. 최근에 사용한 VA→PA 변환 결과를 저장합니다.

### First touch

Anonymous virtual page를 처음 읽거나 쓰는 동작입니다. 이때 kernel이 연결할 physical page를 할당합니다.

## Stressapptest가 사용하는 메모리 단위

### SAT block

Stressapptest의 queue가 상태를 관리하는 메모리 구역입니다. 소스 코드에서는 `page`라고 부르며 기본 크기는 1 MiB입니다.

### Linux page

Kernel과 MMU가 virtual address와 physical address의 연결을 관리하는 단위입니다. AArch64에서는 4·16·64 KiB 등을 사용할 수 있습니다.

### Cache line

<sub><em>Cache line: cache fill, coherency 및 write-back이 처리하는 기본 data 단위입니다. mobile ARM에서는 일반적으로 64 B이며 target implementation에서 최종 확인합니다.</em></sub>

### Working set

프로그램이 일정 시간 동안 반복해서 접근하는 전체 데이터 크기입니다. Cache 용량보다 크면 용량 부족으로 인한 cache miss가 증가합니다.

## Cache 관련 용어

### Cache hit

요청한 데이터가 현재 cache에 있는 상태입니다. 해당 cache보다 아래 계층까지 데이터를 요청할 필요가 없습니다.

### Cache miss/refill

요청한 데이터가 현재 cache에 없어서 L2, SLC, DRAM과 같은 하위 계층에서 cache line을 가져오는 동작입니다.

### Write-Back

<sub><em>Write-Back: CPU store가 cache line을 수정하고 dirty 상태로 표시한 후, eviction 또는 clean 조건에서 하위 cache나 system memory 방향으로 기록하는 정책입니다.</em></sub>

### Write-Through

CPU가 쓴 데이터를 cache에 반영하면서 하위 메모리 계층 방향으로도 전달하는 정책입니다. Write buffer와 하위 cache가 포함될 수 있으므로 LPDDR 쓰기 명령의 발생 시점과 항상 같지는 않습니다.

### Dirty line

현재 cache가 하위 계층보다 최신 데이터를 보유한 cache line입니다. 해당 계층에서 제거하기 전에 최신 데이터를 하위 계층 방향으로 기록해야 합니다.

### Clean line

현재 cache 계층이 하위 coherency 지점보다 최신 수정본을 보유하지 않은 cache line입니다. 다른 cache 계층에는 같은 주소의 dirty line이 존재할 수 있습니다.

### Eviction

<sub><em>Eviction: 새로운 line을 배치하기 위해 기존 victim line을 해당 cache level에서 제거하는 동작입니다. dirty victim은 제거 전에 write-back이 필요합니다.</em></sub>

### Write allocate

<sub><em>Write allocate: store miss에서 cache line과 write ownership을 확보한 후 cache에서 데이터를 수정하는 정책입니다.</em></sub>

### RFO/ownership request

다른 CPU가 가진 공동 복사본을 무효화하고 현재 CPU가 cache line을 수정할 권한을 얻기 위한 요청입니다.

### Prefetch

이후 접근할 것으로 예상되는 cache line을 CPU가 실제 읽기 명령보다 먼저 가져오는 기능입니다. `prfm`은 프로그램이 제공하는 참고 정보이며 hardware prefetcher도 별도로 동작할 수 있습니다.

### Coherency

여러 CPU와 coherent 장치가 같은 physical address의 최신값을 일관되게 읽도록 관리하는 규칙과 protocol입니다.

### Memory ordering/consistency

서로 다른 주소에 대한 읽기·쓰기가 다른 CPU나 장치에 어떤 순서로 보이는지 정하는 규칙입니다. 같은 주소의 coherency와는 별개이며 barrier 또는 atomic 명령이 필요할 수 있습니다.

### Snoop

다른 cache가 특정 physical address의 데이터 또는 쓰기 권한을 보유하는지 확인하고, 필요한 경우 cache 상태를 바꾸는 coherency 요청입니다.

### PoC

Point of Coherency의 약어입니다. 같은 coherency 영역에 속한 CPU와 장치가 동일한 최신 데이터를 관찰하도록 보장되는 메모리 계층의 지점입니다.

### PoU

Point of Unification의 약어입니다. 명령어 fetch와 데이터 접근이 같은 내용을 관찰하도록 합쳐지는 cache 계층의 지점입니다. `dc cvau`의 `u`는 PoU를 의미합니다.

### DMB/DSB/ISB

- DMB: 앞뒤 메모리 접근의 관찰 순서를 제한하는 barrier입니다.
- DSB: 앞선 작업이 지정한 범위까지 완료될 때까지 기다리는 barrier입니다.
- ISB: 이후 명령을 새 상태에서 가져오도록 instruction pipeline을 동기화합니다.

Barrier는 architecture가 정의한 메모리 순서 또는 완료 조건을 적용합니다. Cache clean의 도달 범위는 함께 실행한 cache 관리 명령의 PoU 또는 PoC 조건으로 결정됩니다.

## SoC와 LPDDR

### SLC/LLCC

System Cache 또는 Last-Level Cache의 약어입니다. 여러 CPU cluster와 장치가 공동으로 사용할 수 있는 SoC 내부 cache입니다.

### NoC

Network-on-Chip의 약어입니다. CPU, GPU, NPU, UFS, DMC를 연결하는 SoC 내부 연결망입니다.

### DMC

<sub><em>DMC: DRAM Memory Controller의 약어이며 system request를 LPDDR command로 schedule하고 channel, bank, row 및 column을 선택합니다.</em></sub>

### Channel

독립적으로 메모리 요청을 처리할 수 있는 interface 경로입니다. Physical address를 여러 channel에 나누는 방식은 SoC마다 다릅니다.

### Rank

같은 channel의 command와 address 신호를 공유하고 chip-select 등으로 구분되는 DRAM device 집합입니다.

### Bank/Bank Group

DRAM 내부에서 비교적 독립적으로 row 활성화와 읽기·쓰기를 수행할 수 있는 배열 구역입니다.

### Row/Column

Bank 내부의 cell 위치를 나타내는 주소입니다. Row를 활성화하여 row buffer에 연 뒤 column 위치를 읽거나 씁니다.

### Row hit/miss

- Row hit: 접근할 row가 이미 활성화되어 있습니다.
- Row miss 또는 conflict: 현재 row를 닫고 다른 row를 활성화해야 합니다.

Stressapptest가 분산해서 선택하는 단위는 1 MiB virtual block입니다. 실제 row는 VA→PA 변환과 DMC의 주소 배치 규칙으로 결정됩니다.

### DQ/CA

- DQ: DRAM 데이터를 전송하는 신호입니다.
- CA: DRAM 명령과 주소를 전송하는 신호입니다.

SAT pattern word가 실제 DQ pin에 전달되기까지 cache 처리, DMC data swizzle, 주소 분산 등 여러 단계를 거칩니다.

## Worker와 데이터 검사

### Worker

특정 부하 또는 검사 반복문을 실행하는 pthread입니다. Copy, Check, Invert, File, Network, Disk, CPU Worker 등이 있습니다.

### Valid block

기대 pattern 정보가 있으며 복사의 원본 또는 검사의 대상으로 사용할 수 있는 SAT block입니다.

### Empty block

새 데이터를 쓸 대상으로 사용할 수 있는 SAT block입니다. 할당된 virtual address와 연결된 physical page는 유지되고 `pattern` 정보만 null로 설정됩니다.

### Strict mode

기본 검사 방식입니다. 복사와 I/O 과정에서 checksum으로 데이터를 계속 검사합니다.

### Modified Adler checksum

Stressapptest의 `Crc*` 함수가 사용하는 네 누산기 기반의 빠른 checksum입니다. CRC polynomial과 암호학적 hash 알고리즘은 사용하지 않습니다.

### Slow compare

Checksum이 기대값과 다를 때 64-bit word마다 실제값과 기대값을 비교하여 상세 오류 위치를 찾는 과정입니다.

### Tag mode

각 cache line의 첫 word에 현재 주소로 만든 tag를 저장하여 다른 주소의 데이터가 전달된 오류를 찾는 방식입니다.

### RMW

Read-Modify-Write의 약어입니다. 기존값을 읽고 연산한 뒤 같은 위치에 다시 쓰는 동작입니다. `InvertThread`의 `x = ~x`가 이에 해당합니다.

## OS와 측정 도구

### Anonymous mmap

파일과 연결하지 않고 프로세스의 virtual memory를 확보하는 Linux API입니다. Stressapptest가 일반 메모리를 할당할 때 사용하는 방식입니다.

### Page cache

Linux kernel이 파일 내용을 RAM에 보관하는 기능입니다. CPU의 L1·L2 cache와는 별개의 파일 cache 계층입니다.

### `O_DIRECT`

Filesystem page cache 사용을 최소화하도록 kernel에 요청하는 파일 I/O flag입니다. DMA coherency, CPU cache, LLCC, DRAM controller는 기기의 I/O 구성에 따라 계속 동작합니다.

### PMU

Performance Monitoring Unit의 약어입니다. CPU, cache, NoC, DMC에서 발생한 hardware event 수를 기록하는 장치입니다.

### Logical bandwidth

<sub><em>Logical bandwidth: software가 처리 block 수와 block 크기로 계산한 byte/s입니다. 실제 cache, NoC, DMC 및 LPDDR traffic은 각 계층 counter로 측정합니다.</em></sub>

### LMKD

Android Low Memory Killer Daemon의 약어입니다. 사용 가능한 메모리가 부족하면 stressapptest 또는 다른 프로세스를 종료할 수 있습니다. 이 종료를 hardware 오류와 구분해야 합니다.

### pstore/ramoops

시스템 재시작 또는 kernel panic 전에 kernel 로그를 reserved persistent RAM 등에 남기는 기능입니다. 정상 로그가 중간에 끊긴 문제를 분석할 때 사용합니다.

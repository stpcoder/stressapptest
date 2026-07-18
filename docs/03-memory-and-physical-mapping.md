# 메모리 할당과 virtual/physical mapping

## 기본 메모리 크기 결정

`-M`을 지정하지 않으면 `OsLayer::FindFreeMemSize()`가 target을 계산한다 (`src/os.cc:411`).

일반 small-page 경로의 계산은 다음과 같다.

| total physical memory | target |
|---|---|
| 2 GiB 미만 | total의 약 85% |
| 2 GiB 이상 | total의 약 95% - 192 MiB |

코드는 available physical page 수를 log에 표시한다. target 계산에는 total physical memory 비율을 적용한다. 메모리 사용량이 높은 Android 기기에서 자동값을 사용하면 allocation failure, swap/zram 압박, LMKD kill 또는 system responsiveness 저하가 발생할 수 있다.

모바일 시험에서는 다음 명령 형식으로 `-M`을 명시한다.

```bash
stressapptest -M 512 -s 60 -m 4
```

`--reserve_memory N`은 target 계산 시 최소 N MiB를 system에 남기도록 보정한다. parser가 인식하는 문자열은 underscore를 사용한 `--reserve_memory`이며, built-in help에는 hyphen을 사용한 `--reserve-memory`가 출력된다. 실행 명령에는 parser 문자열을 적용한다.

## Allocation 경로

`AllocateTestMem()`은 다음 순서의 경로를 가진다 (`src/os.cc:508`).

1. 충분한 huge page가 있으면 SysV `SHM_HUGETLB`
2. 32-bit 대용량 경로에서는 POSIX shared memory/dynamic mapping
3. 일반 경로에서는 anonymous private `mmap`
4. 실패 시 4 KiB aligned `memalign`

Android ARM64에서 통상 사용되는 경로는 다음이다.

```c
mmap(NULL, length,
     PROT_READ | PROT_WRITE,
     MAP_PRIVATE | MAP_ANONYMOUS,
     -1, 0)
```

이 mapping에는 운영체제가 관리하는 normal cacheable userspace memory attribute가 적용된다. physical DRAM의 직접 mapping과 non-cacheable attribute 설정은 이 경로에 포함되지 않는다.

<sub><em>Memory attribute: page table과 MAIR를 통해 memory type, cacheability 및 shareability를 지정하는 속성입니다.</em></sub><br>
<sub><em>Normal cacheable memory: CPU cache hierarchy와 coherency protocol을 통해 접근하는 일반 데이터 메모리 유형입니다.</em></sub>

## Reservation과 first touch

anonymous `mmap()` 성공은 virtual address range를 확보했다는 의미다. 각 page의 physical backing은 일반적으로 최초 access 때 page fault를 통해 만들어진다.

stressapptest의 초기 FillThread는 전체 range에 store하며 first-touch phase를 수행한다.

```text
mmap 성공
   ↓
아직 touch하지 않은 virtual page
   ↓ FillThread store
minor page fault
   ↓
kernel이 physical page 선택 및 PTE 설치
   ↓
store가 cache hierarchy에 반영
```

초기 fill이 끝난 뒤에는 대부분의 test range가 resident 상태가 된다. 실행 중 PFN은 Android kernel의 reclaim, migration 및 zram/swap 정책에 따라 변경될 수 있다.

## 세 종류의 주소

```text
Application Virtual Address
          ↓ MMU/TLB/page table
System Physical Address
          ↓ NoC/DMC decoder + interleave/hash
LPDDR channel/rank/bank-group/bank/row/column
```

### Virtual address

프로세스가 pointer로 사용하는 주소다. address space별 page table에 따라 process마다 독립적으로 physical page에 mapping된다.

<sub><em>Virtual address, VA: process address space에서 instruction이 load/store 대상으로 사용하는 주소입니다.</em></sub>

### System physical address

MMU translation 후 interconnect가 사용하는 주소다. 4 KiB Linux page 예시는 다음과 같다.

```text
PA = PFN × 4096 + VA의 하위 12-bit offset
```

page granule이 16 KiB인 system에서는 16 KiB page size와 해당 offset bit 수를 적용한다.

<sub><em>System physical address, PA: MMU translation 이후 CPU와 NoC가 memory transaction에 사용하는 주소입니다.</em></sub><br>
<sub><em>PFN: Physical Frame Number의 약어이며 physical page의 번호입니다.</em></sub>

### DRAM coordinate

DMC가 PA bit를 다시 해석해 channel, rank, bank, row, column을 선택한다. 최신 mobile DMC는 bandwidth와 bank parallelism을 위해 address bit XOR hashing과 interleaving을 사용할 수 있다.

연속 PA의 DRAM coordinate는 vendor DMC의 interleave 및 hash 규칙에 따라 결정된다.

<sub><em>DRAM coordinate: channel, rank, bank group, bank, row 및 column으로 구성되는 DRAM 내부 위치 정보입니다.</em></sub><br>
<sub><em>Interleaving: 연속 주소를 여러 channel 또는 bank에 분산하여 병렬성을 높이는 주소 배치 방식입니다.</em></sub>

## Virtual 연속성과 physical 연속성

`-M 1024`로 얻은 1 GiB virtual range는 연속이다. 이 range를 구성하는 각 Linux page의 PFN은 독립적으로 할당되며 비연속 배치가 가능하다.

또한 SAT block은 virtual offset 기준으로 생성된다.

```text
SAT block 0: VA base + 0 MiB
SAT block 1: VA base + 1 MiB
SAT block 2: VA base + 2 MiB
```

각 1 MiB block은 여러 4/16/64 KiB physical page로 구성되며, 각 page의 PFN은 비연속적으로 배치될 수 있다. random 1 MiB block 선택은 virtual offset 단위의 선택이며 DRAM row 선택은 DMC address mapping 결과로 결정된다.

## `/proc/self/pagemap`

`OsLayer::VirtualToPhysical()`은 `/proc/self/pagemap`에서 PFN을 읽어 PA를 계산한다 (`src/os.cc:141`). 계산 결과는 진단 metadata에 저장되며 worker의 load/store address 생성에는 사용되지 않는다.

Linux 4.2 이후에는 `CAP_SYS_ADMIN`이 없으면 PFN field가 0으로 가려질 수 있다. Android shell/root 정책에 따라 다음 현상이 가능하다.

- file open 자체 실패
- PFN 값이 0으로 마스킹
- SELinux 정책으로 접근 제한
- page migration 때문에 오래된 mapping 정보

PFN을 얻어도 DRAM channel/bank/row는 알 수 없다.

## `--paddr_base`의 의미와 한계

public generic `OsLayer::AllocateTestMem()`은 non-zero `paddr_base`를 지원하지 않고 warning 후 무시한다 (`src/os.cc:514`).

일반 Android build에서 다음 명령의 `paddr_base` 값은 memory allocation target에 반영되지 않는다.

```bash
stressapptest --paddr_base 0x80000000 ...
```

특정 reserved memory나 MMIO를 시험하려면 kernel driver 또는 platform-specific `OsLayer`가 필요하며, arbitrary physical memory를 userspace에 노출하는 것은 안전성과 보안 문제가 크다.

## `--do_page_map`

이 option은 access한 physical 4 KiB page를 bitmap으로 기록한다. 구현 전제 조건은 다음과 같다.

- 4 KiB page granularity
- physical memory가 비교적 0-based
- PFN을 userspace에서 읽을 수 있음
- 최대 physical range가 추정값 안에 있음

16 KiB page Android와 PFN-restricted 환경에서는 신뢰하기 어렵고 assertion 위험도 있다. production phone에서 기본 사용을 권장하지 않는다.

## Channel/DIMM decode option

`--memory_channel`, `--channel_hash`, `--channel_width`는 PA에서 channel/package 이름을 추정해 오류 log를 보강한다.

generic 구현의 제한:

- 1개 또는 2개 channel만 지원
- channel은 지정 mask bit들의 parity/XOR로 선택
- x4 DRAM 미지원
- DIMM/package 모델이 mobile LPDDR topology와 다름
- vendor DMC remap, rank/bank XOR, interleave를 알지 못함

최신 mobile SoC에서 실제 LPDDR package 위치를 출력하려면 vendor address-map 자료를 반영한 `OsLayer::FindDimm()` 구현이 필요하다. generic 구현의 출력은 generic model에 따른 추정값으로 기록한다.

## DMA에는 IOVA가 하나 더 있을 수 있다

UFS, GPU, NPU 등의 device는 IOVA를 사용하며 IOMMU/SMMU가 이를 PA로 변환할 수 있다.

```text
Device IOVA → SMMU → System PA → DMC → LPDDR
```

FileThread의 `O_DIRECT` 적용 범위는 filesystem page cache 우회 요청이다. DMA coherency, CPU cache, SLC, SMMU 및 DMC transaction은 platform I/O 경로에 따라 계속 사용된다.

<sub><em>IOVA: DMA device가 transaction address로 사용하는 I/O virtual address입니다.</em></sub><br>
<sub><em>SMMU: device의 IOVA를 system physical address로 변환하고 접근 권한을 적용하는 System MMU입니다.</em></sub><br>
<sub><em>O_DIRECT: filesystem page cache 사용을 최소화하도록 kernel에 요청하는 file open flag입니다.</em></sub>

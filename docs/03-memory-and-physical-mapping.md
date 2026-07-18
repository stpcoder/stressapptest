# 메모리 할당과 virtual/physical mapping

## 기본 메모리 크기 결정

`-M`을 지정하지 않으면 `OsLayer::FindFreeMemSize()`가 target을 계산한다 (`src/os.cc:411`).

일반 small-page 경로의 계산은 다음과 같다.

| total physical memory | target |
|---|---|
| 2 GiB 미만 | total의 약 85% |
| 2 GiB 이상 | total의 약 95% - 192 MiB |

코드는 available physical page 수도 읽어 log에 표시하지만 target 계산의 기준으로 직접 쓰지 않는다. 따라서 메모리가 이미 많이 사용 중인 Android 기기에서 자동값은 allocation failure, swap/zram 압박, LMKD kill 또는 system responsiveness 저하를 만들 수 있다.

모바일 시험에서는 다음처럼 `-M`을 명시하는 것을 권장한다.

```bash
stressapptest -M 512 -s 60 -m 4
```

`--reserve_memory N`은 target 계산 시 최소 N MiB를 system에 남기도록 보정한다. 단, parser는 underscore인 `--reserve_memory`를 인식하는 반면 built-in help는 hyphen인 `--reserve-memory`를 출력한다. 현재 소스 기준으로 실제 동작하는 것은 underscore 형식이다.

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

이는 normal cacheable userspace memory다. physical DRAM을 직접 map하거나 cache attribute를 non-cacheable로 지정하지 않는다.

## Reservation과 first touch

anonymous `mmap()` 성공은 virtual address range를 확보했다는 의미다. 각 page의 physical backing은 일반적으로 최초 access 때 page fault를 통해 만들어진다.

stressapptest에서는 초기 FillThread가 전체 range에 store하므로 사실상 first-touch phase 역할을 한다.

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

초기 fill이 끝난 뒤에는 대부분의 test range가 resident 상태가 된다. 다만 Android kernel의 reclaim, migration, zram/swap 정책에 따라 전체 실행 동안 동일한 PFN이 유지된다고 보장할 수는 없다.

## 세 종류의 주소

```text
Application Virtual Address
          ↓ MMU/TLB/page table
System Physical Address
          ↓ NoC/DMC decoder + interleave/hash
LPDDR channel/rank/bank-group/bank/row/column
```

### Virtual address

프로세스의 pointer다. 같은 숫자의 virtual address라도 다른 process에서는 다른 physical page를 가리킬 수 있다.

### System physical address

MMU translation 후 interconnect가 사용하는 주소다. 4 KiB Linux page 예시는 다음과 같다.

```text
PA = PFN × 4096 + VA의 하위 12-bit offset
```

실제 page granule이 16 KiB라면 offset bit 수와 PFN 계산이 달라진다.

### DRAM coordinate

DMC가 PA bit를 다시 해석해 channel, rank, bank, row, column을 선택한다. 최신 mobile DMC는 bandwidth와 bank parallelism을 위해 address bit XOR hashing과 interleaving을 사용할 수 있다.

따라서 PA가 연속이어도 하나의 LPDDR row에 순차 배치된다고 단정할 수 없다.

## Virtual 연속성과 physical 연속성

`-M 1024`로 얻은 1 GiB virtual range는 연속이다. 그러나 이를 구성하는 Linux page들의 PFN은 일반적으로 불연속일 수 있다.

또한 SAT block은 virtual offset 기준으로 생성된다.

```text
SAT block 0: VA base + 0 MiB
SAT block 1: VA base + 1 MiB
SAT block 2: VA base + 2 MiB
```

각 1 MiB block 내부에 들어 있는 4/16/64 KiB physical page가 서로 연속이라는 보장은 없다. 그러므로 “random 1 MiB block”은 random DRAM row 선택과 같은 뜻이 아니다.

## `/proc/self/pagemap`

`OsLayer::VirtualToPhysical()`은 `/proc/self/pagemap`에서 PFN을 읽어 PA를 계산한다 (`src/os.cc:141`). 이것은 진단용 metadata이며 memory access 자체에 사용되지 않는다.

Linux 4.2 이후에는 `CAP_SYS_ADMIN`이 없으면 PFN field가 0으로 가려질 수 있다. Android shell/root 정책에 따라 다음 현상이 가능하다.

- file open 자체 실패
- PFN 값이 0으로 마스킹
- SELinux 정책으로 접근 제한
- page migration 때문에 오래된 mapping 정보

PFN을 얻어도 DRAM channel/bank/row는 알 수 없다.

## `--paddr_base`의 의미와 한계

public generic `OsLayer::AllocateTestMem()`은 non-zero `paddr_base`를 지원하지 않고 warning 후 무시한다 (`src/os.cc:514`).

따라서 다음 명령은 일반 Android build에서 특정 physical address를 test하도록 보장하지 않는다.

```bash
stressapptest --paddr_base 0x80000000 ...
```

특정 reserved memory나 MMIO를 시험하려면 kernel driver 또는 platform-specific `OsLayer`가 필요하며, arbitrary physical memory를 userspace에 노출하는 것은 안전성과 보안 문제가 크다.

## `--do_page_map`

이 option은 access한 physical 4 KiB page를 bitmap으로 기록한다. 하지만 구현은 다음을 가정한다.

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

최신 mobile SoC에서는 vendor address-map 자료로 `OsLayer::FindDimm()`을 새로 구현하지 않는 한 결과를 실제 LPDDR package 위치로 해석하면 안 된다.

## DMA에는 IOVA가 하나 더 있을 수 있다

UFS, GPU, NPU 같은 device는 CPU virtual address 대신 IOVA를 사용하고 IOMMU/SMMU가 이를 PA로 변환할 수 있다.

```text
Device IOVA → SMMU → System PA → DMC → LPDDR
```

FileThread의 `O_DIRECT`는 filesystem page cache를 우회하려는 option이지 CPU cache, SLC, SMMU 또는 DMC를 우회하는 기능이 아니다.

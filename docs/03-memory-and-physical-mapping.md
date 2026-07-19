# 테스트 메모리 준비 과정

stressapptest는 먼저 프로세스가 사용할 virtual memory 영역을 확보합니다. 이후 초기 데이터를 쓰는 과정에서 kernel이 실제 physical page를 연결합니다. 이 장에서는 테스트 메모리의 크기와 주소가 변환되는 과정을 설명합니다.

## 테스트할 메모리 크기 정하기

`-M`을 지정하지 않으면 `OsLayer::FindFreeMemSize()`가 테스트할 메모리 크기를 자동으로 계산합니다 (`src/os.cc:411`).

일반 page를 사용하는 경우의 계산 기준은 다음과 같습니다.

| 기기에 설치된 전체 RAM | 자동으로 선택하는 테스트 크기 |
|---|---|
| 2 GiB 미만 | 전체 RAM의 약 85% |
| 2 GiB 이상 | 전체 RAM의 약 95%에서 192 MiB를 뺀 크기 |

로그에는 현재 사용할 수 있는 physical page 수도 표시됩니다. 그러나 자동 테스트 크기는 전체 RAM의 비율을 기준으로 계산합니다. 이미 메모리를 많이 사용하는 Android 기기에서 자동값을 적용하면 메모리 할당 실패, swap·zram 사용 증가, LMKD에 의한 프로세스 종료, 시스템 응답 저하가 발생할 수 있습니다.

모바일 시험에서는 다음 명령 형식으로 `-M`을 명시한다.

```bash
stressapptest -M 512 -s 60 -m 4
```

`--reserve_memory N`은 테스트 크기를 계산할 때 운영체제와 다른 프로그램이 사용할 메모리를 최소 N MiB 남깁니다. 실제 옵션 처리 코드는 밑줄이 있는 `--reserve_memory`를 인식합니다. 프로그램의 도움말에는 `--reserve-memory`로 표시되지만 실행 명령에는 `--reserve_memory`를 사용해야 합니다.

## 메모리 할당 방식

`AllocateTestMem()`은 다음 순서로 메모리 할당을 시도합니다 (`src/os.cc:508`).

1. 충분한 huge page가 있으면 SysV `SHM_HUGETLB`
2. 32-bit 환경에서 큰 메모리가 필요하면 POSIX shared memory 또는 동적 매핑
3. 일반 환경에서는 anonymous private `mmap`
4. 실패 시 4 KiB aligned `memalign`

Android ARM64에서는 일반적으로 다음 `mmap()` 경로를 사용합니다.

```c
mmap(NULL, length,
     PROT_READ | PROT_WRITE,
     MAP_PRIVATE | MAP_ANONYMOUS,
     -1, 0)
```

이 메모리에는 운영체제가 정한 일반 cacheable userspace memory 속성이 적용됩니다. 특정 physical DRAM 주소를 직접 연결하거나 cache를 사용하지 않도록 바꾸는 동작은 포함되지 않습니다.

### Android/Linux에서 사용하는 mmap 방식

> **파일:** `src/os.cc` · **함수:** `OsLayer::AllocateTestMem()` · **기준:** `73b9df2`

```cpp
if (!use_hugepages_ && !use_posix_shm_) {
  if (sysconf(_SC_PAGESIZE) >= 4096) {
    void *map_buf = mmap(NULL, length, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (map_buf != MAP_FAILED) {
      buf = map_buf;
      mmapped_allocation_ = true;
    }
  }
}
```

**코드 설명:** `mmap()`이 반환하는 값은 프로세스가 사용할 virtual address의 시작 위치입니다. 이 호출은 DRAM channel, bank, row를 지정하지 않습니다. 각 virtual page에 연결할 physical page는 kernel의 page allocator가 선택합니다. 실제 연결은 해당 page에 처음 접근할 때 이루어집니다.

<sub><em>Memory attribute: page table과 MAIR를 통해 memory type, cacheability 및 shareability를 지정하는 속성입니다.</em></sub><br>
<sub><em>Normal cacheable memory: CPU cache hierarchy와 coherency protocol을 통해 접근하는 일반 데이터 메모리 유형입니다.</em></sub>

## Virtual address를 physical address로 변환하는 코드

> **파일:** `src/os.cc` · **함수:** `OsLayer::VirtualToPhysical()` · **기준:** `73b9df2`

```cpp
uint64 frame, paddr, pfnmask, pagemask;
int pagesize = sysconf(_SC_PAGESIZE);
off_t off = ((uintptr_t)vaddr) / pagesize * 8;
int fd = open(kPagemapPath, O_RDONLY);

if (lseek(fd, off, SEEK_SET) != off || read(fd, &frame, 8) != 8)
  return 0;

if (!(frame & (1ULL << 63)) || (frame & (1ULL << 62)))
  return 0;

pfnmask = ((1ULL << 55) - 1);
pagemask = pagesize - 1;
paddr = ((frame & pfnmask) * pagesize) | ((uintptr_t)vaddr & pagemask);
return paddr;
```

**코드 설명:** `/proc/self/pagemap`에서 해당 page가 RAM에 있는지와 swap으로 이동했는지를 확인합니다. RAM에 있으면 PFN에 page 내부 offset을 더하여 physical address를 계산합니다. Android kernel이 PFN 공개를 제한하면 PFN이 0으로 표시되거나 파일 열기·읽기가 실패할 수 있습니다. 계산 결과는 system physical address이며 LPDDR channel·bank·row 위치는 아닙니다.

## Virtual memory 예약과 physical page 할당

anonymous `mmap()`의 성공은 사용할 virtual address 범위를 확보했다는 의미입니다. 각 virtual page에 연결되는 physical page는 일반적으로 처음 읽거나 쓸 때 발생하는 page fault를 통해 준비됩니다.

stressapptest의 초기 `FillThread`는 테스트 범위 전체에 데이터를 씁니다. 이때 아직 physical page가 없는 주소에서는 kernel이 physical page를 할당하고 page table을 설정합니다.

```text
mmap 성공
   ↓
아직 접근하지 않은 virtual page
   ↓ FillThread가 데이터 쓰기
minor page fault
   ↓
kernel이 physical page를 선택하고 PTE 설정
   ↓
쓴 데이터가 cache 계층에 반영
```

초기 데이터 쓰기가 끝나면 테스트 범위의 대부분이 RAM에 존재합니다. 실행 중에는 Android kernel의 메모리 회수, page 이동, zram·swap 정책에 따라 PFN이 달라질 수 있습니다.

## 주소가 변환되는 단계

```text
프로그램이 사용하는 virtual address
          ↓ MMU·TLB·page table
system physical address
          ↓ NoC·DMC의 주소 해석과 주소 분산 규칙
LPDDR channel·rank·bank group·bank·row·column
```

### Virtual address

프로그램의 pointer에 저장되는 주소입니다. 프로세스마다 독립된 page table을 사용하므로, 같은 virtual address라도 서로 다른 physical page에 연결될 수 있습니다.

<sub><em>Virtual address, VA: 프로세스의 address space에서 CPU 명령이 읽기·쓰기 대상으로 사용하는 주소입니다.</em></sub>

### System physical address

MMU가 주소를 변환한 뒤 SoC 내부 연결망이 사용하는 주소입니다. Linux page 크기가 4 KiB인 경우는 다음과 같이 계산합니다.

```text
PA = PFN × 4096 + VA의 하위 12-bit offset
```

Linux page 크기가 16 KiB이면 16 KiB와 그에 맞는 page 내부 offset bit 수를 적용합니다.

<sub><em>System physical address, PA: MMU translation 이후 CPU와 NoC가 memory transaction에 사용하는 주소입니다.</em></sub><br>
<sub><em>PFN: Physical Frame Number의 약어이며 physical page의 번호입니다.</em></sub>

### LPDDR channel·bank·row 좌표

DMC는 physical address의 bit를 해석하여 channel, rank, bank, row, column을 선택합니다. 최신 모바일 DMC는 여러 channel과 bank를 동시에 사용하기 위해 주소 bit를 XOR하거나 연속 주소를 여러 위치에 나누어 배치할 수 있습니다.

따라서 physical address가 연속이어도 LPDDR 내부 위치가 연속이라고 판단할 수 없습니다. 실제 위치는 SoC 제조사의 DMC 주소 배치 규칙에 따라 결정됩니다.

<sub><em>DRAM coordinate: channel, rank, bank group, bank, row 및 column으로 구성되는 DRAM 내부 위치 정보입니다.</em></sub><br>
<sub><em>Interleaving: 연속 주소를 여러 channel 또는 bank에 분산하여 병렬성을 높이는 주소 배치 방식입니다.</em></sub>

## Virtual address와 physical address의 연속성

`-M 1024`로 확보한 1 GiB virtual address 범위는 연속입니다. 그러나 이 범위를 구성하는 각 Linux page는 서로 떨어진 physical page에 연결될 수 있습니다.

SAT block도 virtual address의 offset을 기준으로 나눕니다.

```text
SAT block 0: VA base + 0 MiB
SAT block 1: VA base + 1 MiB
SAT block 2: VA base + 2 MiB
```

각 1 MiB block은 여러 Linux page로 구성되며, 각 page의 PFN은 연속하지 않을 수 있습니다. stressapptest가 임의의 1 MiB block을 고르는 것은 virtual address 범위에서 block을 선택하는 동작입니다. 특정 DRAM row를 직접 선택하는 동작은 아닙니다.

## `/proc/self/pagemap`

`OsLayer::VirtualToPhysical()`은 `/proc/self/pagemap`에서 PFN을 읽어 physical address를 계산합니다 (`src/os.cc:141`). 이 값은 오류 위치를 기록하는 진단 정보로만 사용합니다. Worker가 읽고 쓸 주소를 정하는 데에는 사용하지 않습니다.

Linux 4.2 이후에는 `CAP_SYS_ADMIN` 권한이 없을 때 PFN이 0으로 가려질 수 있습니다. Android의 shell 권한과 보안 정책에 따라 다음 문제가 발생할 수 있습니다.

- 파일 열기 실패
- PFN 값이 0으로 마스킹
- SELinux 정책으로 접근 제한
- page가 이동하여 이전 주소 정보가 더 이상 유효하지 않음

PFN을 얻더라도 SoC 제조사의 DMC 주소 배치 규칙이 없으면 DRAM channel·bank·row를 계산할 수 없습니다.

## `--paddr_base`의 의미와 한계

공개 저장소의 공통 `OsLayer::AllocateTestMem()` 구현은 0이 아닌 `paddr_base`를 지원하지 않습니다. 경고를 출력한 뒤 해당 값을 무시합니다 (`src/os.cc:514`).

따라서 일반 Android build에서는 다음 명령의 `paddr_base`가 메모리 할당 위치에 반영되지 않습니다.

```bash
stressapptest --paddr_base 0x80000000 ...
```

특정 reserved memory나 MMIO 영역을 시험하려면 kernel driver 또는 해당 SoC에 맞춘 `OsLayer` 구현이 필요합니다. 임의의 physical memory를 userspace에 노출하면 시스템 손상과 보안 문제가 발생할 수 있습니다.

## `--do_page_map`

이 옵션은 접근한 4 KiB physical page를 bitmap에 기록합니다. 현재 구현은 다음 조건을 전제로 합니다.

- 4 KiB page granularity
- physical address 범위가 0에 가까운 주소에서 시작함
- userspace에서 PFN을 읽을 수 있음
- 최대 physical address가 프로그램이 예상한 범위 안에 있음

16 KiB page를 사용하는 Android 또는 PFN 공개가 제한된 환경에서는 결과를 신뢰하기 어렵고 프로그램이 중단될 수도 있습니다. 일반 제품 시험에서는 기본 사용을 권장하지 않습니다.

## Channel과 DIMM을 추정하는 옵션

`--memory_channel`, `--channel_hash`, `--channel_width`는 physical address에서 channel과 package 이름을 추정하여 오류 로그에 추가합니다.

공통 구현의 제한은 다음과 같습니다.

- 1개 또는 2개 channel만 지원
- 지정한 address bit의 parity 또는 XOR로 channel 선택
- x4 DRAM 미지원
- DIMM·package 구조가 모바일 LPDDR 구조와 다름
- SoC 제조사가 적용한 DMC 재배치, rank·bank XOR, interleave 규칙을 반영하지 못함

최신 모바일 SoC에서 실제 LPDDR 위치를 출력하려면 SoC 제조사의 address map을 반영하여 `OsLayer::FindDimm()`을 구현해야 합니다. 공통 구현이 출력하는 위치는 일반적인 계산 모형에 따른 추정값입니다.

## DMA에서 사용하는 IOVA

UFS, GPU, NPU와 같은 장치는 IOVA를 사용할 수 있습니다. IOMMU 또는 SMMU가 IOVA를 system physical address로 변환합니다.

```text
장치가 사용하는 IOVA → SMMU → system physical address → DMC → LPDDR
```

`FileThread`의 `O_DIRECT`는 Linux filesystem page cache를 가능한 범위에서 우회하도록 요청하는 옵션입니다. DMA coherency, CPU cache, SLC, SMMU, DMC는 기기의 I/O 구성에 따라 계속 사용됩니다.

<sub><em>IOVA: DMA device가 transaction address로 사용하는 I/O virtual address입니다.</em></sub><br>
<sub><em>SMMU: device의 IOVA를 system physical address로 변환하고 접근 권한을 적용하는 System MMU입니다.</em></sub><br>
<sub><em>O_DIRECT: filesystem page cache 사용을 최소화하도록 kernel에 요청하는 file open flag입니다.</em></sub>

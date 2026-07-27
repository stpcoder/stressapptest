# 테스트 메모리 준비 과정

stressapptest는 먼저 프로세스가 사용할 virtual memory 영역을 확보합니다. 이후 초기 데이터를 쓰는 과정에서 kernel이 실제 physical page를 연결합니다. 이 장에서는 테스트 메모리의 크기와 주소가 변환되는 과정을 설명합니다.

## 테스트할 메모리 크기 정하기

`-M`의 기본값은 `OsLayer::FindFreeMemSize()`가 자동으로 계산합니다 (`src/os.cc:411`).

일반 page를 사용하는 경우의 계산 기준은 다음과 같습니다.

| 기기에 설치된 전체 RAM | 자동으로 선택하는 테스트 크기 |
|---|---|
| 2 GiB 미만 | 전체 RAM의 약 85% |
| 2 GiB 이상 | 전체 RAM의 약 95%에서 192 MiB를 뺀 크기 |

로그에는 현재 available physical page 수도 표시됩니다. 자동 테스트 크기는 전체 RAM의 비율을 기준으로 계산합니다. 메모리 사용량이 큰 Android 기기에서는 메모리 할당 실패, swap·zram 사용 증가, LMKD 종료와 시스템 응답 저하가 발생할 수 있습니다.

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

이 메모리에는 운영체제가 정한 일반 cacheable userspace memory 속성이 적용됩니다. Physical page는 kernel page allocator가 배정하며 CPU access는 cache hierarchy를 통과합니다.

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

**코드 설명:** `mmap()`은 프로세스가 사용할 virtual address의 시작 위치를 반환합니다. Kernel page allocator가 각 virtual page의 physical page를 선택합니다. DRAM channel, bank와 row는 이후 memory-controller address mapping에 따라 결정됩니다. 실제 page 연결은 해당 page의 최초 접근 시점에 이루어집니다.

<sub><em>Memory attribute: page table과 MAIR를 통해 memory type, cacheability 및 shareability를 지정하는 속성입니다.</em></sub>
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

**코드 설명:** `/proc/self/pagemap`에서 page의 RAM·swap 상태를 확인합니다. RAM page는 PFN과 page 내부 offset으로 system physical address를 계산합니다. Android kernel의 PFN 접근 정책에 따라 0 또는 읽기 오류가 반환될 수 있습니다. LPDDR channel·bank·row 변환에는 DMC address map을 추가로 적용합니다.

## Virtual memory 예약과 physical page 할당

anonymous `mmap()`의 성공은 사용할 virtual address 범위를 확보했다는 의미입니다. 각 virtual page에 연결되는 physical page는 일반적으로 처음 읽거나 쓸 때 발생하는 page fault를 통해 준비됩니다.

stressapptest의 초기 `FillThread`는 테스트 범위 전체에 데이터를 씁니다. 이때 아직 physical page가 없는 주소에서는 kernel이 physical page를 할당하고 page table을 설정합니다.

```text
mmap 성공
   ↓
최초 접근 전의 virtual page
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

<sub><em>System physical address, PA: MMU translation 이후 CPU와 NoC가 memory transaction에 사용하는 주소입니다.</em></sub>
<sub><em>PFN: Physical Frame Number의 약어이며 physical page의 번호입니다.</em></sub>

### LPDDR channel·bank·row 좌표

DMC는 physical address의 bit를 해석하여 channel, rank, bank, row, column을 선택합니다. 최신 모바일 DMC는 여러 channel과 bank를 동시에 사용하기 위해 주소 bit를 XOR하거나 연속 주소를 여러 위치에 나누어 배치할 수 있습니다.

따라서 physical address가 연속이어도 LPDDR 내부 위치가 연속이라고 판단할 수 없습니다. 실제 위치는 SoC 제조사의 DMC 주소 배치 규칙에 따라 결정됩니다.

<sub><em>DRAM coordinate: channel, rank, bank group, bank, row 및 column으로 구성되는 DRAM 내부 위치 정보입니다.</em></sub>
<sub><em>Interleaving: 연속 주소를 여러 channel 또는 bank에 분산하여 병렬성을 높이는 주소 배치 방식입니다.</em></sub>

## `lpddr-v1` 주소 변환 프로필

이 저장소는 `--dram-map lpddr-v1` 옵션으로 physical address를 DRAM 좌표로 변환합니다. 변환 코드는 `src/dram_address.h`에 있습니다. 옵션을 지정하지 않으면 로그의 DRAM 좌표에는 `unknown`이 기록됩니다.

아래 식에서 `P[n]`은 system physical address의 n번 bit를 의미합니다. `^`는 XOR 연산입니다. 두 bit로 구성된 값은 bit 0과 bit 1을 결합하여 0부터 3까지의 값으로 만듭니다.

| DRAM 필드 | `lpddr-v1` 계산식 | 출력 형식 |
|---|---|---|
| Byte offset | `P[4:0]` | 16진수 계산 기준 |
| Column | `{P[14:12], P[7:5]}` | 16진수 |
| Channel | `P[9:8]` | 10진수 0~3 |
| Subchannel | `P[10]` | 10진수 0~1 |
| Row | `P[33:18]` | 16진수 |
| Bank group bit 0 | `1 ^ P[11] ^ P[20] ^ P[29] ^ P[30]` | `bg[0]` |
| Bank group bit 1 | `P[15] ^ P[19] ^ P[21] ^ P[24]` | `bg[1]` |
| Bank bit 0 | `P[16] ^ P[21] ^ P[23] ^ P[30]` | `bank[0]` |
| Bank bit 1 | `P[17] ^ P[19] ^ P[23] ^ P[27] ^ P[30]` | `bank[1]` |
| Rank | `0` | 이 프로필의 검증 범위 |

최종 bank group과 bank 값은 다음과 같이 계산합니다.

```text
bg   = bg[0]   + 2 × bg[1]
bank = bank[0] + 2 × bank[1]
```

예를 들어 physical address `0x90a5edc3c`에는 다음 결과가 적용됩니다.

```text
ch:0,rk:0,sc:1,bg:1,bank:2,row:4297,col:29
```

`row`와 `col`은 16진수 문자열로 출력됩니다. 위 예시의 `row:4297`은 10진수 4297이 아니라 16진수 `0x4297`을 의미합니다.

### 프로필 적용 범위

`lpddr-v1`의 식은 저장소의 검증 vector 전체와 일치합니다. Rank 1 주소가 검증 vector에 포함되지 않아 이 프로필의 rank는 0으로 고정됩니다. Bank group과 bank의 XOR 탭은 현재 vector를 만족하는 프로필 식입니다. 추가 주소만으로 같은 결과를 만드는 다른 XOR 식이 존재할 수 있으므로 target의 controller 설정 또는 별도 주소 표본으로 확인합니다.

Memory-controller의 주소 배치는 boot 설정, interleave 설정과 memory topology에 따라 달라질 수 있습니다. `lpddr-v1`은 명령행에서 선택한 실행에만 적용됩니다. 다른 주소 배치를 사용하는 target에는 별도의 `DramAddressMapProfile`과 decode 함수를 추가합니다.

<sub><em>XOR hash: 여러 address bit를 XOR하여 channel 또는 bank 선택 bit를 만드는 주소 분산 규칙입니다.</em></sub>
<sub><em>Address-map profile: system physical address를 DRAM 좌표로 변환하는 bit 배치와 XOR 식의 묶음입니다.</em></sub>

### 오류 주소에 적용되는 physical address

Memory mismatch 로그의 virtual address는 오류가 포함된 64-bit word의 시작 주소입니다. Physical address는 `read`와 `expected`를 byte 단위로 비교하여 처음 다른 byte의 virtual address를 `/proc/self/pagemap`으로 변환한 값입니다.

```text
word virtual address
 + first mismatching byte offset
 = mismatching byte virtual address
 → /proc/self/pagemap
 = 오류 로그의 physical address
 → lpddr-v1
 = ch, rk, sc, bg, bank, row, col
```

따라서 로그의 virtual address와 physical address는 같은 byte 위치를 직접 표현하지 않을 수 있습니다. 64-bit word의 다섯 번째 byte가 처음 다르면 physical address의 page offset은 virtual word 시작 offset보다 4만큼 큽니다.

## Virtual address와 physical address의 연속성

`-M 1024`로 확보한 1 GiB virtual address 범위는 연속입니다. 각 Linux page의 physical 위치는 kernel page allocator가 개별적으로 결정합니다.

SAT block도 virtual address의 offset을 기준으로 나눕니다.

```text
SAT block 0: VA base + 0 MiB
SAT block 1: VA base + 1 MiB
SAT block 2: VA base + 2 MiB
```

각 1 MiB block은 여러 Linux page로 구성되며 PFN 배치는 kernel page allocator가 결정합니다. Stressapptest는 virtual address 범위에서 1 MiB block을 선택합니다. DRAM row는 각 page의 physical address와 memory-controller mapping으로 결정됩니다.

## `/proc/self/pagemap`

`OsLayer::VirtualToPhysical()`은 `/proc/self/pagemap`에서 PFN을 읽어 physical address를 계산합니다 (`src/os.cc:141`). 이 값은 오류 위치 진단에 사용합니다. Worker의 접근 주소는 queue가 선택한 virtual block으로 결정됩니다.

Linux 4.2 이후에는 `CAP_SYS_ADMIN` 권한이 없을 때 PFN이 0으로 가려질 수 있습니다. Android의 shell 권한과 보안 정책에 따라 다음 문제가 발생할 수 있습니다.

- 파일 열기 실패
- PFN 값이 0으로 마스킹
- SELinux 정책으로 접근 제한
- page migration으로 이전 주소 정보가 만료됨

PFN을 얻더라도 SoC 제조사의 DMC 주소 배치 규칙이 없으면 DRAM channel·bank·row를 계산할 수 없습니다.

## `--paddr_base`의 의미와 한계

공개 저장소의 공통 `OsLayer::AllocateTestMem()`은 `paddr_base == 0` 조건의 anonymous allocation을 지원합니다. 다른 값을 입력하면 경고를 출력하고 anonymous allocation을 계속합니다 (`src/os.cc:514`).

일반 Android build에서 다음 명령은 anonymous allocation을 사용합니다.

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

이 기능은 4 KiB page와 PFN 접근을 전제로 합니다. 16 KiB page 또는 PFN 제한 환경에서는 대상 build의 page 크기 처리와 권한을 먼저 검증합니다.

## Channel과 DIMM을 추정하는 옵션

`--memory_channel`, `--channel_hash`, `--channel_width`는 physical address에서 channel과 package 이름을 추정하여 오류 로그에 추가합니다.

공통 구현의 제한은 다음과 같습니다.

- 1개 또는 2개 channel만 지원
- 지정한 address bit의 parity 또는 XOR로 channel 선택
- x4 DRAM 미지원
- DIMM·package 구조가 모바일 LPDDR 구조와 다름
- SoC 제조사의 DMC 재배치, rank·bank XOR와 interleave 규칙은 별도 적용

최신 모바일 SoC에서 실제 LPDDR 위치를 출력하려면 SoC 제조사의 address map을 반영하여 `OsLayer::FindDimm()`을 구현해야 합니다. 공통 구현이 출력하는 위치는 일반적인 계산 모형에 따른 추정값입니다.

## DMA에서 사용하는 IOVA

UFS, GPU, NPU와 같은 장치는 IOVA를 사용할 수 있습니다. IOMMU 또는 SMMU가 IOVA를 system physical address로 변환합니다.

```text
장치가 사용하는 IOVA → SMMU → system physical address → DMC → LPDDR
```

`FileThread`의 `O_DIRECT`는 Linux filesystem page cache를 가능한 범위에서 우회하도록 요청하는 옵션입니다. DMA coherency, CPU cache, SLC, SMMU, DMC는 기기의 I/O 구성에 따라 계속 사용됩니다.

<sub><em>IOVA: DMA device가 transaction address로 사용하는 I/O virtual address입니다.</em></sub>
<sub><em>SMMU: device의 IOVA를 system physical address로 변환하고 접근 권한을 적용하는 System MMU입니다.</em></sub>
<sub><em>O_DIRECT: filesystem page cache 사용을 최소화하도록 kernel에 요청하는 file open flag입니다.</em></sub>

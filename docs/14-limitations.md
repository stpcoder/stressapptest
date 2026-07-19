# 모바일 환경에서 알아둘 제한사항

이 장에서는 stressapptest가 직접 제어하거나 확인할 수 없는 항목을 정리합니다. LPDDR channel·bank·row를 선택하는 주소 규칙과 controller의 명령 처리 순서를 확인하려면 SoC 문서 또는 hardware counter가 필요합니다.

## 소스 코드만으로 확인할 수 없는 항목

소스 코드에서 확인할 수 있는 것은 virtual address를 선택하고 접근하는 순서입니다. 다음 항목은 SoC 문서 또는 실제 측정이 필요합니다.

- 교체할 cache line을 선택하는 기준
- Write allocate와 연속 쓰기 최적화의 전환 조건
- Prefetch 거리와 동시에 요청하는 데이터양
- SLC가 하위 cache의 데이터를 포함하는지와 SLC 분할 방식
- NoC의 QoS와 요청 순서 변경
- DMC의 명령 배치와 읽기·쓰기 묶음 처리
- Physical address에서 channel·bank·row를 계산하는 규칙
- LPDDR 명령과 실제 pin 신호

## Userspace에서 제어할 수 있는 범위

stressapptest는 Android application processor에서 일반 userspace 읽기·쓰기 명령을 사용합니다. 각 항목을 제어하는 주체는 다음과 같습니다.

| 항목 | 제어 주체 |
|---|---|
| VA→PA 변환과 접근 권한 | kernel page table과 MMU |
| cacheability와 shareability | kernel page table과 MAIR |
| cache replacement와 write-back | CPU/cache microarchitecture |
| DMC register와 주소 배치 | kernel·firmware·SoC 제조사 driver |
| refresh, timing, training | DMC/PHY firmware와 hardware |
| DVFS | kernel governor, firmware 및 PMIC |

Stressapptest가 직접 정하는 것은 virtual address 선택, 접근 순서, 데이터 pattern, Worker 수입니다.

<sub><em>Userspace load/store: process virtual address와 운영체제가 지정한 memory attribute를 사용하여 수행하는 CPU memory access입니다.</em></sub><br>
<sub><em>MAIR: Memory Attribute Indirection Register의 약어이며 page-table attribute index에 대응하는 memory type을 정의합니다.</em></sub>

Firmware, bootloader, PHY 진단 도구는 controller와 PHY를 직접 제어할 수 있습니다. Stressapptest는 OS scheduling, physical page 할당, coherency, I/O DMA, 온도, DVFS가 함께 동작하는 운영체제 환경을 시험합니다.

## 메모리 크기를 자동으로 정할 때의 위험

RAM이 2 GiB 이상이면 기본 테스트 크기는 전체 RAM의 95%에서 192 MiB를 뺀 값입니다. 현재 사용할 수 있는 메모리와 Android service가 필요로 하는 여유 메모리를 충분히 반영하지 않습니다.

양산 휴대폰에서는 다음 문제가 발생할 수 있습니다.

- zram 접근이 과도하게 반복됨
- LMKD가 stressapptest 또는 다른 프로그램을 종료함
- `system_server`와 다른 프로그램에 메모리가 부족해짐
- 테스트 메모리 할당 실패
- 화면과 시스템 응답 정지

따라서 `-M`으로 테스트 크기를 직접 지정해야 합니다.

## Generic ARM의 ErrorPoll 동작

`ErrorPollThread`가 실행되더라도 공개 저장소의 공통 `OsLayer::ErrorPoll()`은 0을 반환합니다. Corrected ECC, SoC RAS, secure firmware 오류는 자동으로 수집하지 않습니다.

`--monitor_mode`와 `--no_errors`의 결과를 해석할 때 이 제한을 반영해야 합니다.

> **파일:** `src/os_factory.cc`, `src/os.cc` · **함수:** `OsLayerFactory()`, `OsLayer::ErrorPoll()` · **기준:** `73b9df2`

```cpp
OsLayer *OsLayerFactory(
    const std::map<std::string, std::string> &options) {
  OsLayer *os = 0;
  os = new OsLayer();

  if (!os) {
    logprintf(0, "Process Error: Can't allocate memory\n");
    return 0;
  }
  return os;
}

int OsLayer::ErrorPoll() {
  return 0;
}
```

**코드 설명:** 공개 저장소의 공통 build는 SoC별 `OsLayer`를 선택하지 않습니다. `ErrorPollThread`가 1초마다 실행되어도 공통 `ErrorPoll()`은 hardware 오류를 수집하지 않습니다. Android의 RAS, EDAC, firmware 로그를 읽으려면 해당 SoC에 맞는 `OsLayer` 구현이 필요합니다.

## ARM64 오류 재검사의 제한

공통 AArch64 구현에서는 `has_clflush_`가 false입니다. 따라서 데이터 불일치 후 `OsLayer::Flush()`가 실제 cache 관리 명령을 실행하지 않을 수 있습니다. 첫 번째 값과 다시 읽은 값의 차이만으로 읽기 오류를 분류한 결과에는 이 제한이 있습니다.

## `dc cvau`의 적용 범위

`InvertThread`의 ARM64 cache 관리 코드는 PoU 방향의 data cache clean과 instruction cache 무효화로 구성됩니다. LPDDR 요청의 발생 여부와 시점은 하위 cache와 DMC 상태에 따라 결정됩니다.

## 소스 버전에 따른 `-W` 동작 차이

현재 분석한 GitHub master에는 ARM64 NEON 복사 코드가 있습니다. AOSP mirror 또는 이전 package는 C 코드로 대체될 수 있습니다. 실행 파일을 만든 저장소와 commit을 확인해야 합니다.

ARM64의 `-W`는 AArch64 `ld1`과 `st1`이 수행하는 일반 cacheable 읽기·쓰기로 분석해야 합니다. x86의 `movntdq`에 적용되는 non-temporal store 특성을 ARM64 결과에 적용하면 안 됩니다.

## Physical address 변환의 제한

- 권한이 없으면 `/proc/self/pagemap`의 PFN이 0으로 표시될 수 있습니다.
- `paddr`에는 block의 첫 주소에 대응하는 physical address만 저장됩니다.
- Block 안의 Linux page가 physical address에서도 연속이라는 보장은 없습니다.
- 실행 중 kernel이 physical page를 이동할 수 있습니다.
- `--do_page_map`은 4 KiB page와 0에 가까운 physical address 시작점을 가정합니다.
- 공통 channel 계산은 1~2개 channel과 parity·XOR 규칙만 지원합니다.
- `--paddr_base`는 공통 build에서 무시됩니다.

## Pattern width 제한

Pattern 이름의 32·64·128·256은 32-bit word를 반복하는 범위를 나타냅니다. 실제 DQ 폭, burst length, channel 폭은 DMC와 LPDDR 설정에서 별도로 정합니다.

## 기본 Worker가 만드는 읽기·쓰기 조합

지속적으로 쓰기만 수행하는 메모리 Worker는 없습니다. 초기 `FillThread`는 쓰기 중심이고, `CopyThread`는 읽기와 쓰기를 함께 수행하며, `InvertThread`는 read-modify-write를 수행합니다.

## 대상 block을 검사하는 시점

기본 복사는 원본의 checksum을 계산하면서 대상에 데이터를 씁니다. 같은 복사 작업에서 대상을 다시 읽지는 않습니다. 따라서 대상에 쓴 데이터의 오류는 해당 block을 이후 원본으로 선택하거나 마지막 전체 검사를 수행할 때 발견합니다.

## Checksum 한계

Modified Adler checksum은 데이터 변화를 빠르게 찾기 위한 검사값입니다. 암호학적 checksum의 충돌 방지 성능이나 CRC polynomial 방식의 특성을 제공하지 않으므로 서로 다른 데이터가 같은 checksum을 만들 가능성이 있습니다. Word 단위 상세 비교는 checksum이 기대값과 다를 때 실행합니다.

## 임의 선택 방식의 범위

FineLock queue는 고정된 초기값을 사용하는 pseudo-random 계산으로 block 후보를 고릅니다. Pattern 선택은 `random()`의 상태와 thread 실행 순서의 영향을 받습니다. 주소 선택 순서가 암호학적으로 임의이거나 모든 physical row에 균등하게 분포한다는 보장은 없습니다.

`DiskThread`는 `srandom(time(NULL))`을 호출하므로 프로세스 전체가 사용하는 `random()` 상태에도 영향을 줄 수 있습니다.

## 온라인 CPU 수와 허용 CPU 수의 차이

기본 `CopyThread` 수는 `_SC_NPROCESSORS_ONLN` 값을 사용하고 CPU affinity 대상은 `sched_getaffinity()` 결과를 사용합니다. Android cpuset이 제한되어 있으면 Worker 수가 실제 실행 가능한 CPU 수보다 많을 수 있습니다.

CPU affinity 코드에는 사용 가능한 CPU 번호가 연속이라는 가정이 일부 포함되어 있습니다. 중간 CPU 번호가 빠진 mask에서는 경고 또는 설정 실패가 발생할 수 있습니다.

## CPU frequency test는 x86 전용

`--cpu_freq_test`는 `/dev/cpu/*/msr`와 x86 CPUID를 사용합니다. ARM64 모바일에서는 사용할 수 없습니다.

## Block device 옵션의 구현상 주의점

- `-d --destructive`는 실제 저장 장치 데이터를 손상시킬 수 있습니다.
- `DiskThread`의 asynchronous direct I/O를 사용하려면 build에 libaio가 필요합니다.
- 장치 크기는 cache 추정값의 3배보다 커야 합니다.
- `--random-threads` 실행 경로에는 initialized 상태 설정 함수가 빠진 것으로 보입니다.
- `O_DIRECT` 실패 후 사용하는 page cache 정리 방식은 시스템 전체 I/O에 영향을 줄 수 있습니다.

## ARM64 시간 측정 register

AArch64 `GetTimestamp()`는 `CNTVCT_EL0`를 읽습니다. Kernel이 userspace의 virtual counter 접근을 허용하지 않는 기기에서는 trap 또는 illegal instruction이 발생할 수 있는지 확인해야 합니다.

## 일시 정지 관련 값에 0을 사용할 때의 문제

`--printsec 0` 또는 `--pause_delay 0`은 다음 실행 시점을 계산하는 내부 나눗셈에 문제가 될 수 있습니다. 0보다 큰 값을 사용해야 합니다.

## 도움말과 실제 option 이름의 차이

- parser: `--reserve_memory`
- help: `--reserve-memory`
- help 누락: `-c`, `--coarse_grain_lock`, `--tag_mode`, `--do_page_map`
- Upstream README의 “processor당 두 thread” 설명은 현재 코드의 기본값인 `memory_threads = online CPUs`와 다름

항상 대상 실행 파일과 같은 commit의 옵션 처리 코드를 기준으로 해야 합니다.

## 결과 해석 기준

```text
stressapptest의 데이터 불일치
  → 메모리 관련 경로에서 데이터가 바뀐 증거
  → CPU core, cache, coherency, NoC, DMC, DRAM, 프로그램 경로를 순서대로 구분

stressapptest 실행 중 종료
  → 프로그램 종료 또는 시스템 재시작이 관찰된 결과
  → OOM, LMKD, 온도, watchdog, kernel, RAS 원인 구분

stressapptest MB/s
  → 프로그램이 계산한 논리적 처리량
  → DMC·LPDDR counter 측정값과 함께 분석
```

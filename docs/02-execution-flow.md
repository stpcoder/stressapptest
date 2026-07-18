# 시작부터 종료까지 실행 순서

## 전체 timeline

```text
argument parse
   ↓
OS/CPU 정보 초기화
   ↓
memory 크기 결정 및 mmap
   ↓
pattern variant와 expected checksum 생성
   ↓
1 MiB block metadata/queue 생성
   ↓
8개 FillThread로 전체 memory write       ┐
   ↓                                      │ -s 이전
physical 진단/tag 및 valid/empty 분류     ┘
   ↓
실행 worker spawn
   ↓
-s countdown 동안 copy/check/invert/I/O
   ↓
worker stop/join
   ↓
8개 CheckThread로 전체 valid block 검사  ← -s 이후
   ↓
통계/오류 출력 및 memory 해제
```

## 1. Argument parse

`Sat::ParseArgs()`가 option을 순서대로 읽는다 (`src/sat.cc:794`). GNU `getopt`를 쓰지 않고 문자열 비교 macro를 사용한다.

구현상 특징:

- 숫자는 `strtoull(..., base=0)`로 읽어 `0x` hexadecimal 입력도 가능하다.
- 일부 signed field에도 unsigned parse 결과를 대입한다.
- `-f`, `-d`, `-n`, `--memory_channel`은 반복 지정할 수 있다.
- 알 수 없는 option은 version/help를 출력하고 exit 1한다.
- `-h`, `--help`는 version/help를 출력하고 exit 0한다.

### 실제 초기화 순서

> **파일:** `src/sat.cc` · **함수:** `Sat::Initialize()` · **기준:** `73b9df2`

```cpp
os_ = OsLayerFactory(options);
if (!os_->Initialize())
  return false;

if (!CheckEnvironment())
  return false;

if (!AllocateMemory())
  return false;

if (!InitializePatterns())
  return false;

pages_ = size_ / page_length_;
```

**해석:** CPU 수와 memory 기본값은 `OsLayer` 초기화 이후 확정됩니다. memory allocation이 성공한 다음 pattern checksum과 SAT block 수가 구성됩니다. 따라서 `-s` countdown이 시작되기 전에 큰 memory allocation과 전체 fill이 수행됩니다.

## 2. 환경 및 기본값 결정

- runtime 기본값: 20초
- block 크기: 1 MiB
- fill thread: 8
- copy thread: 미지정 시 online logical CPU 수
- strict transaction check: 켜짐
- warm `-W`: 꺼짐
- pause/resume: 600초마다 15초 pause

`-M`이 0이면 `FindFreeMemSize()`가 total physical memory 비율을 기준으로 target을 정한다. available memory는 log 출력에 사용되며 target 산정의 주 기준에는 포함되지 않는다. Android 실행에서는 `-M`을 명시하여 할당량을 제한한다.

## 3. Virtual memory 확보

일반 Android/Linux 경로는 다음 `mmap()`이다.

```c
mmap(NULL, length,
     PROT_READ | PROT_WRITE,
     MAP_PRIVATE | MAP_ANONYMOUS,
     -1, 0)
```

이 시점에는 virtual address range가 예약된다. physical backing page는 이후 FillThread의 store에서 발생하는 page fault와 first touch를 통해 할당된다.

<sub><em>Anonymous mmap: file backing 없이 process virtual address range를 확보하는 Linux memory mapping 방식입니다.</em></sub><br>
<sub><em>First touch: 예약된 virtual page에 최초로 접근하여 kernel의 physical backing page 할당을 유도하는 동작입니다.</em></sub>

## 4. Pattern 초기화

15개 pattern family 각각에 대해:

- non-inverted/inverted
- 32/64/128/256 logical width

variant를 만든다. 총 slot은 15 × 8 = 120개다. weight 0인 variant object도 생성되며 random selection 대상에서는 제외된다.

각 variant는 첫 4 KiB에 대한 expected modified-Adler checksum을 미리 계산한다.

## 5. 전체 memory fill

처음에는 모든 `page_entry.pattern`이 null이므로 empty다. 기본 8개의 FillThread가 block을 하나씩 가져와:

1. weighted random pattern 선택
2. 1 MiB 전체를 64-bit store로 채움
3. pattern pointer와 last CPU 기록
4. valid 상태로 반환

이 단계는 모든 test memory를 실제로 touch하며 write-heavy traffic을 만든다. `-s` timer가 시작되기 전이다.

## 6. Physical 진단과 valid/empty 비율 설정

모든 block을 한 번 채운 다음 다시 각 block을 가져와:

- 첫 virtual address의 possible physical address를 `/proc/self/pagemap`으로 조회
- generic region tag 계산
- `--do_page_map`이면 4 KiB 단위 bitmap 갱신
- 기본 fine-lock queue에서는 약 2/5를 empty, 약 3/5를 valid로 분류

`empty`는 queue에서 destination으로 선택 가능한 상태를 의미한다. 해당 1 MiB allocation과 physical backing은 유지된다.

<sub><em>Valid block: 기대 pattern metadata를 보유하며 source 또는 검증 대상으로 사용할 수 있는 SAT block입니다.</em></sub><br>
<sub><em>Empty block: destination write 대상으로 사용할 수 있도록 pattern metadata가 해제된 SAT block입니다.</em></sub>

## 7. Timed worker 실행

`Sat::Run()`이 worker를 만들고 `pthread_create()`한 뒤에 `-s` countdown을 시작한다 (`src/sat.cc:1884`).

> **파일:** `src/sat.cc` · **함수:** `Sat::Run()` · **기준:** `73b9df2`

```cpp
InitializeThreads();
SpawnThreads();

const time_t start = time(NULL);
const time_t end = start + runtime_seconds_;
```

**해석:** `InitializeThreads()`는 C++ worker object를 구성하고 `SpawnThreads()`는 각 object에 대해 `pthread_create()`를 호출합니다. `-s` 시간 측정의 기준점은 worker spawn 이후에 설정됩니다. 초기 FillThread와 종료 후 CheckThread 시간은 timed interval에 포함되지 않습니다.

기본 CopyThread는 반복해서:

```text
GetValid(source)
GetEmpty(destination)
CrcCopyPage(source → destination)
PutValid(destination)
PutEmpty(source)
sched_yield()
```

을 수행한다.

## 8. Pause/resume power step

기본 `--pause_delay 600 --pause_duration 15`다. 해당 시간이 되면 `power_spike_status`에 속한 worker를 pause했다가 동시에 resume한다.

`power_spike_status` 그룹에는 주로 CopyThread, FileThread, DiskThread 및 CPU frequency monitor가 포함된다. `continuous_status` 그룹의 worker는 해당 pause 대상에서 제외된다. 600초 미만의 test에서는 기본 pause 시점에 도달하지 않는다.

## 9. 종료와 final check

timer 만료, signal, excessive error 조건이 발생하면 worker를 stop/join한다. 그 다음 기본 8개의 CheckThread가 valid block을 모두 꺼내 checksum/slow compare하고 empty로 바꾼다.

따라서 copy destination write 오류는 copy 직후 즉시 발견되지 않더라도:

- 그 block이 나중에 source가 되었을 때
- check worker가 읽었을 때
- 마지막 full check에서

발견될 수 있다.

## 10. Process exit code

- 내부 fatal status 또는 data error가 하나라도 있으면 exit 1
- 오류가 없으면 exit 0

reboot, kernel panic, watchdog reset, process SIGKILL은 정상적인 final result 출력 전에 process가 사라질 수 있으므로 pstore/kernel log/LMKD log를 별도로 수집해야 한다.

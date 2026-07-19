# I/O·CPU Worker 종류와 동작

이 장에서는 메모리 복사 외에 파일, 네트워크, 저장 장치, CPU 연산, cache coherency를 시험하는 Worker를 설명합니다. 각 Worker는 해당 옵션을 지정할 때만 생성됩니다.

## FileThread (`-f`)

`-f path` 하나마다 `FileThread` 하나를 만듭니다. 기본 `disk_pages_`는 8입니다. Block 크기가 기본값인 1 MiB이면 한 번의 작업에서 8 MiB를 파일에 쓴 뒤 다시 8 MiB를 읽습니다.

### 파일을 여는 방식

```text
O_RDWR | O_CREAT | O_SYNC | O_DIRECT
```

`O_DIRECT`를 사용한 파일 열기가 `EINVAL`로 실패하면 `O_DIRECT`를 제외하고 다시 엽니다. 이후 filesystem page cache를 정리하는 경로를 활성화합니다.

> **파일:** `src/worker.cc` · **함수:** `FileThread::OpenFile()` · **기준:** `73b9df2`

```cpp
int flags = O_RDWR | O_CREAT | O_SYNC;
int fd = open(filename_.c_str(), flags | O_DIRECT, 0644);
if (O_DIRECT != 0 && fd < 0 && errno == EINVAL) {
  fd = open(filename_.c_str(), flags, 0644);
  os_->ActivateFlushPageCache();
}
```

**코드 설명:** 첫 번째 `open()`은 direct I/O를 요청합니다. Filesystem이 이를 지원하지 않아 `EINVAL`을 반환하면 buffered I/O로 바꾸고 이후 page cache 정리를 요청합니다. `O_DIRECT`는 Linux의 파일 page cache에 적용되는 옵션이며 CPU data cache를 끄지는 않습니다.

<sub><em>O_SYNC: write system call의 데이터와 필요한 metadata가 backing storage에 동기화되도록 요청하는 file open flag입니다.</em></sub>
<sub><em>O_DIRECT: filesystem page cache 사용을 최소화하도록 kernel에 요청하는 file open flag입니다.</em></sub>
<sub><em>Page cache: Linux kernel이 file 데이터를 RAM에 보관하여 file I/O를 처리하는 cache 계층입니다.</em></sub>

### 파일 한 번을 처리하는 순서

```text
valid SAT block 8개 가져오기
 → 기본 검사 방식이면 원본 checksum 확인
 → 각 512 B sector에 magic·block·sector·반복 번호 기록
 → 파일 시작 위치부터 순차 쓰기
 → 원본 block을 empty 상태로 반환
 → 파일 시작으로 seek
 → empty SAT block 8개에 순차 읽기
 → sector 정보를 검사하고 원래 pattern 복원
 → 기본 검사 방식이면 block checksum 확인
 → 대상 block을 valid 상태로 반환
```

`FileThread`는 UFS 또는 저장 장치의 DMA와 메모리·NoC 접근을 동시에 발생시킵니다. `O_DIRECT`는 Linux page cache에만 적용됩니다. DMA coherency와 CPU L1·L2·SLC의 동작은 기기의 I/O 구성에 따라 계속 발생합니다.

> **파일:** `src/worker.cc` · **함수:** `FileThread::ReadPages()` · **기준:** `73b9df2`

```cpp
if (!ReadPageFromFile(fd, &dst)) {
  PutEmptyPage(&dst);
  return false;
}

SectorValidatePage(page_recs_[i], &dst, i);

if (strict) {
  int errors = CrcCheckPage(&dst);
  errorcount_ += errors;
}
PutValidPage(&dst);
```

**코드 설명:** 파일에서 읽은 데이터는 empty SAT block에 저장합니다. Sector 정보를 확인하고 기본 검사 방식에서는 pattern checksum도 검사합니다. 검사가 끝난 block은 valid 상태로 바꾸어 다시 SAT 메모리 영역에서 사용할 수 있게 합니다.

<sub><em>DMA: device가 CPU의 개별 load/store 개입 없이 system memory와 데이터를 전송하는 기능입니다.</em></sub>

`--filesize`는 한 번에 처리할 파일 크기를 byte로 지정합니다. 내부에서는 `disk_pages = filesize / SAT block 크기`로 block 수를 계산하며 최소값은 한 block입니다.

## NetworkThread (`-n`, `--listen`)

통신에는 TCP port 19996을 사용합니다.

> **파일:** `src/worker.cc` · **함수:** `NetworkThread::Work()` · **기준:** `73b9df2`

```cpp
result = result && sat_->GetValid(&src);
result = result && sat_->GetEmpty(&dst);

if (strict)
  CrcCheckPage(&src);

result = result && SendPage(sock, &src);
dst.pattern = src.pattern;
dst.lastcpu = sched_getcpu();
result = result && ReceivePage(sock, &dst);

if (strict)
  CrcCheckPage(&dst);

result = result && sat_->PutValid(&dst);
result = result && sat_->PutEmpty(&src);
```

**코드 설명:** Valid block을 상대 기기로 전송하고 상대 기기가 그대로 돌려보낸 1 MiB를 empty 대상 block에 받습니다. 기본 검사 방식에서는 전송 전 원본과 수신 후 대상을 각각 검사합니다.

### 데이터를 되돌려 보내는 쪽

`--listen`은 `0.0.0.0:19996`에서 연결을 기다리고 연결마다 `NetworkSlaveThread`를 만듭니다.

수신 측은 512 B 단위로 정렬된 임시 buffer에 SAT block 하나를 받은 뒤 같은 데이터를 송신 측으로 돌려보냅니다. Pattern 검사는 송신 측에서 수행합니다.

### 테스트 데이터를 보내는 쪽

`-n ipaddr` 하나마다 `NetworkThread` 하나를 만듭니다. 프로그램 시작 후 15초를 기다린 뒤 상대 기기에 연결합니다.

```text
valid 원본 + empty 대상 가져오기
 → 기본 검사 방식이면 원본 checksum 확인
 → 원본 block 하나를 TCP로 전송
 → 상대 기기가 돌려보낸 block을 대상에 수신
 → 기본 검사 방식이면 대상 checksum 확인
 → 대상은 valid, 기존 원본은 empty로 변경
```

네트워크 Worker는 CPU 복사, socket buffer, kernel network stack, DMA, NIC 또는 Wi-Fi 장치를 함께 사용합니다. 따라서 결과를 LPDDR controller만의 부하로 해석할 수 없습니다.

`--tag_mode`와 함께 사용할 수 없습니다.

## DiskThread (`-d`)

> **파일:** `src/worker.cc` · **함수:** `DiskThread::OpenDevice()` · **기준:** `73b9df2`

```cpp
int flags = O_RDWR | O_SYNC | O_LARGEFILE;
int fd = open(device_name_.c_str(), flags | O_DIRECT, 0);
if (O_DIRECT != 0 && fd < 0 && errno == EINVAL) {
  fd = open(device_name_.c_str(), flags, 0);
  os_->ActivateFlushPageCache();
}
```

**코드 설명:** 시험 대상을 `O_RDWR`로 열기 때문에 block device에 쓸 수 있는 권한을 얻습니다. 기본값인 `--non_destructive`에서는 쓰기 단계를 실행하지 않습니다. 그러나 대상 경로를 잘못 지정하거나 옵션을 변경하면 저장된 데이터가 손상될 수 있습니다.

`-d device-or-file`은 장치 또는 파일의 임의 위치를 읽고 쓰는 `DiskThread` 하나를 만듭니다. `DiskThread`는 sector·block 상태표와 asynchronous I/O를 사용합니다. 반면 `FileThread`는 SAT block 단위로 파일의 처음부터 순차 처리합니다.

<sub><em>Asynchronous I/O: I/O 요청 제출과 완료 수집을 분리하여 여러 요청을 동시에 계류시키는 방식입니다.</em></sub>

### 기본 읽기 동작과 쓰기 활성화 조건

`--destructive`를 지정하지 않으면 쓰기 단계를 실행하지 않고 임의 위치를 읽기만 합니다. 이때 기존 저장 장치의 데이터는 SAT pattern과 비교하지 않습니다.

`--destructive`를 지정하면 임의의 block에 SAT pattern을 쓴 뒤, 여러 I/O 요청을 진행하고 해당 block을 다시 읽어 검사합니다.

> 휴대폰의 실제 block device에 `--destructive`를 사용하면 filesystem, userdata, boot partition을 복구할 수 없게 손상시킬 수 있습니다.

### 주요 I/O 설정값

- sector와 주소 정렬 단위: 512 B
- 기본 읽기 block: 512 B
- 쓰기 block: 지정하지 않으면 읽기 block과 같은 크기
- 기본 disk cache: Worker 생성 코드 기준 16 MiB
- 동시에 진행하는 I/O 수: cache에 들어가는 block 수의 약 150%
- 필요한 장치 크기: cache 크기의 3배 초과
- asynchronous 읽기·쓰기 사용 조건: build에 libaio 포함

### RandomDiskThread

`--random-threads N`은 각 `DiskThread`에 N개의 추가 읽기 Worker를 만듭니다. 이 Worker들은 공동 `DiskBlockTable`에서 초기화가 끝난 block을 임의로 선택하여 검사합니다.

현재 분석한 코드의 주 `DiskThread`는 block을 준비한 뒤 상태를 읽는 `block->initialized()`를 호출합니다 (`src/worker.cc:2948`). 그러나 해당 실행 경로에는 상태를 설정하는 `block->set_initialized()` 호출이 없습니다. 따라서 상태표의 initialized 값이 설정되지 않아 `RandomDiskThread`가 검사할 block을 얻지 못할 수 있습니다. 이 옵션을 사용할 때에는 대상 build에서 상태 변경 과정을 먼저 확인해야 합니다.

## CpuStressThread (`-C`)

> **파일:** `src/worker.cc` · **함수:** `CpuStressThread::Work()` · **기준:** `73b9df2`

```cpp
do {
  os_->CpuStressWorkload();
  YieldSelf();
} while (IsReadyToRun());
```

**코드 설명:** CPU 연산을 반복하고 매 반복이 끝날 때 scheduler에 다른 thread를 실행할 기회를 줍니다. 이 Worker는 메모리 pattern의 합격·불합격을 판정하지 않습니다.

`-C N`은 N개의 CPU 연산 Worker를 만듭니다.

공통 ARM·Linux 구현은 100개의 `double` 배열을 사용하여 moving average 형태의 부동소수점 계산을 100,000,000회 반복합니다 (`src/os.cc:904`).

특성:

- 계산 배열이 작아 L1 cache에 머물 가능성이 큽니다.
- 주된 부하는 CPU의 부동소수점 또는 vector 연산입니다.
- 연산 pipeline, CPU 동적 전력, 온도, DVFS에 영향을 줍니다.
- `CopyThread`와 함께 사용하면 메모리 접근과 연산이 동시에 많은 조건을 만듭니다.
- Worker 상태는 계산 반복문의 실행 성공 여부로 기록합니다.

## CpuCacheCoherencyThread (`--cc_test`)

> **파일:** `src/worker.cc` · **함수:** `CpuCacheCoherencyThread::Work()` · **기준:** `73b9df2`

```cpp
for (int i = 0; i < cc_inc_count_; i++) {
  r = SimpleRandom(r);
  int cline_num = r % cc_cacheline_count_;
  int offset = cc_thread_num_;
  (cc_cacheline_data_[cline_num].num[offset])++;
}

int cc_global_num = 0;
for (int cline_num = 0; cline_num < cc_cacheline_count_; cline_num++) {
  int offset = cc_thread_num_;
  cc_global_num += cc_cacheline_data_[cline_num].num[offset];
  cc_cacheline_data_[cline_num].num[offset] = 0;
}
```

**코드 설명:** 각 thread는 cache line 크기의 구조체를 pseudo-random 방식으로 선택합니다. 구조체 안에서 자신에게 배정된 byte 값을 증가시킨 뒤 전체 합이 기대값과 같은지 검사합니다. 실제 코드에는 홀수 번호 thread와 cache line에서 byte 위치의 순서를 반대로 정하는 조건도 있습니다.

설정한 CPU 수만큼 thread를 만들고 각 thread를 지정한 CPU에서 실행합니다.

여러 cache line 크기의 구조체 중 하나를 pseudo-random 방식으로 선택하고, 자신의 byte counter를 반복해서 증가시킵니다. 이후 모든 구조체에서 해당 counter를 더한 값이 `cc_inc_count`와 같은지 확인하고 0으로 초기화합니다.

목적:

- 여러 CPU core 사이에서 shared cache line의 쓰기 권한 이동
- Snoop과 cache line 무효화
- Cache coherency protocol의 정상 동작 검사

이 시험은 작은 공동 데이터의 cache line 쓰기 권한을 여러 CPU core 사이에서 반복해서 이동시킵니다. 주된 부하는 snoop, cache line 무효화, 쓰기 권한 이동입니다. SLC와 DRAM까지 전달되는 요청의 비율은 SoC의 coherency 구현에 따라 달라집니다.

<sub><em>Cache-line ownership: 특정 core가 cache line을 수정할 수 있도록 coherency protocol이 부여한 권한 상태입니다.</em></sub>
<sub><em>Snoop: 다른 cache가 해당 address의 data 또는 ownership을 보유하는지 조회하는 coherency transaction입니다.</em></sub>

관련 옵션은 다음과 같습니다.

- `--cc_line_count`: 공동으로 사용하는 cache line 크기 구조체 수, 기본값 2
- `--cc_line_size`: 자동으로 확인한 cache line 크기를 사용하지 않고 직접 지정
- `--cc_inc_count`: 한 번의 검사에서 값을 증가시키는 횟수, 기본값 1000

## ErrorPollThread

> **파일:** `src/worker.cc` · **함수:** `ErrorPollThread::Work()` · **기준:** `73b9df2`

```cpp
do {
  errorcount_ += os_->ErrorPoll();
  os_->ErrorWait();
} while (IsReadyToRun());
```

**코드 설명:** SoC 오류 확인 함수를 반복해서 호출하며 기본 대기 시간은 1초입니다. 공통 Android·Linux `OsLayer::ErrorPoll()`은 항상 0을 반환합니다. 따라서 SoC별 구현이 없으면 실제 ECC 또는 RAS 오류를 수집하지 않습니다.

기본으로 하나 생성되며 `OsLayer::ErrorPoll()`을 약 1초마다 호출합니다.

공개 저장소의 공통 `OsLayer::ErrorPoll()`은 항상 0을 반환합니다 (`src/os.cc:739`). 현재 Android ARM 공통 build는 SoC가 보고하는 corrected ECC 또는 RAS 오류를 자동으로 읽지 않습니다.

따라서 모바일 시험에서는 다음 정보를 별도로 수집해야 합니다.

- kernel RAS/EDAC/vendor memory error log
- pstore/ramoops
- watchdog/reboot reason
- DMC/LLCC error register
- secure firmware/SoC-specific diagnostics

`--no_errors`는 `ErrorPollThread`를 생성하지 않게 합니다. `CopyThread`, `CheckThread`, 마지막 전체 검사의 pattern 검사는 그대로 유지됩니다.

## CpuFreqThread

> **파일:** `src/worker.cc` · **함수:** `CpuFreqThread::CanRun()` · **기준:** `73b9df2`

```cpp
#if defined(STRESSAPPTEST_CPU_X86_64) || defined(STRESSAPPTEST_CPU_I686)
  // TSC, invariant TSC, APERF/MPERF 지원을 CPUID로 검사한다.
  return true;
#else
  logprintf(0, "Process Error: "
               "cpu_freq_test is only supported on X86 processors.\n");
  return false;
#endif
```

**코드 설명:** 공개 구현은 x86의 CPUID와 MSR을 사용합니다. ARM64 Android에서 `--cpu_freq_test`를 지정하면 실행 환경 검사에 실패합니다.

`--cpu_freq_test`는 TSC, APERF, MPERF MSR을 읽는 x86 전용 기능입니다. AArch64에서는 `CanRun()`이 false를 반환하여 초기화가 실패합니다.

Android ARM의 CPU 주파수는 cpufreq sysfs, tracepoint, SoC 제조사 profiler, simpleperf, Perfetto로 측정해야 합니다. `--cpu_freq_test`는 AArch64에서 지원하지 않는 옵션입니다.

## 오류 감시 전용 방식

> **파일:** `src/sat.cc` · **함수:** `Sat::Initialize()` · **기준:** `73b9df2`

```cpp
if (monitor_mode_) {
  logprintf(5, "Log: Running in monitor-only mode. "
               "Will not allocate any memory nor run any stress test. "
               "Only polling ECC errors.\n");
  return true;
}
```

**코드 설명:** Monitor mode는 테스트 메모리 할당, pattern 준비, 메모리 Worker 생성을 모두 건너뜁니다. `ErrorPollThread`만 실행하며 공통 build에서는 앞 절에서 설명한 대로 실제 SoC 오류를 수집하지 않습니다.

`--monitor_mode`는 테스트 메모리를 할당하지 않고 `ErrorPollThread`만 실행합니다.

공통 ARM의 `ErrorPoll()`은 항상 0을 반환합니다. Android에서 ECC 오류 감시 기능으로 사용하려면 해당 SoC에 맞는 `OsLayer::ErrorPoll()` 구현을 연결해야 합니다.

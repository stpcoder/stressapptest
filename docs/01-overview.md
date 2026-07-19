# stressapptest의 작동 원리

## 이 도구가 하는 일

stressapptest는 여러 Worker가 메모리와 I/O를 반복해서 읽고 쓰게 하면서 데이터 오류를 검사하는 프로그램입니다. 메모리 처리 속도와 함께 부하가 걸린 상태에서 데이터가 정확하게 유지되는지를 확인합니다.

<sub><em>Transaction: Worker가 block을 가져온 시점부터 읽기·쓰기·검사를 마치고 queue에 반환할 때까지의 작업 단위입니다.</em></sub><br>
<sub><em>Pattern: 메모리 block에 반복해서 기록하는 데이터 배열과 해당 배열의 기대 checksum 정보입니다.</em></sub>

일반적인 메모리 대역폭 측정 도구와 stressapptest의 주된 목적은 다음과 같습니다.

| 도구 유형 | 우선 처리 항목 |
|---|---|
| 메모리 대역폭 측정 도구 | 단위 시간당 데이터 전송량 측정 |
| stressapptest | 부하 실행 중 데이터 오류 검사 |

stressapptest의 실행 시간에는 checksum 계산, queue 잠금, block 선택, CPU 실행권 양보가 포함됩니다. 프로그램이 출력하는 처리량은 이러한 작업을 모두 포함하여 Worker가 처리한 데이터 양입니다. 실제 LPDDR 대역폭은 DMC 계측값으로 확인합니다.

<sub><em>Throughput: Worker가 단위 시간에 처리했다고 계산한 논리적 데이터 양입니다.</em></sub><br>
<sub><em>Scheduler yield: 실행 중인 thread가 CPU 실행 기회를 scheduler에 자발적으로 반환하는 동작입니다.</em></sub>

## 주요 구성 요소

```text
main()
 └─ Sat
     ├─ OsLayer
     │   ├─ 메모리 할당
     │   ├─ CPU/affinity 정보
     │   ├─ virtual address→physical address 진단
     │   ├─ cache 관리 명령
     │   └─ SoC 오류 확인
     ├─ PatternList
     │   └─ Pattern 조합 + 기대 checksum
     ├─ FineLockPEQueue 또는 PageEntryQueue
     │   └─ page_entry[각 SAT block]
     ├─ WorkerStatus
     │   ├─ power_spike_status
     │   └─ continuous_status
     └─ WorkerThread를 상속한 Worker 종류
```

`main()`은 `옵션 확인 → 초기화 → 실행 → 결과 출력 → 자원 정리` 순서로 동작합니다 (`src/main.cc:20`).

## 프로그램 실행 순서

> **파일:** `src/main.cc` · **함수:** `main()` · **기준:** `73b9df2`

```cpp
Sat *sat = SatFactory();

if (!sat->ParseArgs(argc, argv)) {
  sat->bad_status();
} else if (!sat->Initialize()) {
  sat->bad_status();
} else if (!sat->Run()) {
  sat->bad_status();
}
sat->PrintResults();
if (!sat->Cleanup()) {
  sat->bad_status();
}
```

**코드 설명:** `Sat` 객체가 프로그램의 전체 실행 상태를 관리합니다. `Initialize()`는 운영체제 정보, 테스트 메모리, 데이터 pattern, block 관리 queue를 준비합니다. `Run()`은 Worker를 만들고 실행합니다. `PrintResults()`는 실행 결과와 오류 수를 출력하고, `Cleanup()`은 할당한 자원을 해제합니다.

> **파일:** `src/sat.cc` · **함수:** `Sat::InitializeThreads()` · **기준:** `73b9df2`

```cpp
for (int i = 0; i < memory_threads_; i++) {
  CopyThread *thread = new CopyThread();
  thread->InitThread(total_threads_++, this, os_, patternlist_,
                     &power_spike_status_);
  memory_vector->insert(memory_vector->end(), thread);
}
workers_map_.insert(make_pair(kMemoryType, memory_vector));
```

**코드 설명:** `-m N`을 지정하면 `CopyThread` 객체 N개를 만듭니다. `-m`을 지정하지 않으면 온라인 상태의 논리 CPU 수를 사용합니다. File, Network, Check, Invert, Disk 및 CPU Worker도 각 옵션에서 지정한 개수만큼 별도의 목록에 추가됩니다.

## 메모리 부하 과정

```text
                    선택 비율에 따라 Pattern 선택
                            ↓
empty block ──FillThread──→ valid block
    ↑                          │
    │                          │ 원본
    │                          ↓
    └────── CopyThread ─── 대상
              읽기 + checksum + 쓰기
```

CopyThread는 queue에서 읽을 원본 block과 쓸 대상 block을 하나씩 가져옵니다. 복사가 끝나면 block 상태는 다음과 같이 바뀝니다.

- 대상 block은 원본과 같은 pattern 정보를 가진 valid block이 됩니다.
- 복사가 끝난 원본 block은 다음 쓰기에 사용할 수 있는 empty block이 됩니다.

이 과정을 반복하면 같은 데이터 pattern이 테스트 메모리 안에서 계속 다른 block으로 이동합니다.

## 메모리에서 사용하는 크기 단위

| 단위 | 기본 크기 | 코드에서 사용하는 목적 |
|---|---:|---|
| 64 B | `kCacheLineSize` | cache line과 주소 tag 간격 |
| 512 B | 저장 장치 sector 정보 | `FileThread`와 `DiskThread`의 sector 단위 |
| 4 KiB | checksum 검사 구간 | 기대 checksum을 계산하고 비교하는 내부 단위 |
| 1 MiB | `kSatPageSize` | queue가 관리하는 기본 SAT block |
| `-M` MiB | 사용자가 지정 | 전체 테스트 메모리 크기 |

1 MiB SAT block 하나에는 64 B cache line 16,384개와 4 KiB checksum 구간 256개가 포함됩니다.

## Worker 종류

별도의 옵션을 지정하지 않았을 때 각 작동 단계에서 생성되는 Worker는 다음과 같습니다.

| 작동 단계 | 생성되는 Worker와 역할 |
|---|---|
| 초기 데이터 쓰기 | FillThread 8개가 모든 block에 pattern 기록 |
| block 상태 설정 | 전체 block 중 약 2/5를 empty, 약 3/5를 valid로 설정 |
| 설정 시간 동안 실행 | 온라인 상태의 논리 CPU 수만큼 CopyThread 생성 |
| 오류 상태 확인 | ErrorPollThread 1개 생성. Generic ARM에서는 항상 오류 0개 반환 |
| 마지막 전체 검사 | CheckThread 8개가 남은 valid block 검사 |

초기 데이터 쓰기와 마지막 전체 검사는 `-s`로 지정한 실행 시간에 포함되지 않습니다. 전체 실행 시간은 초기 데이터 쓰기, 설정 시간 동안의 Worker 실행, 마지막 전체 검사 시간을 모두 합한 값입니다.

## CPU 수와 Worker 배치

`OsLayer::Initialize()`는 `_SC_NPROCESSORS_ONLN`으로 온라인 상태의 논리 CPU 수를 확인합니다. `-m`을 지정하지 않으면 이 값을 CopyThread 수로 사용합니다 (`src/os.cc:108`, `src/sat.cc:148`).

CopyThread 수와 CpuStressThread 수의 합이 사용 가능한 CPU 수 이하이면 각 Worker를 특정 CPU에 고정합니다. Worker 수가 더 많으면 CPU 고정을 생략하고 Android/Linux scheduler가 여러 Worker를 번갈아 실행합니다.

Android에서는 다음 항목을 따로 기록해야 합니다.

- 시스템에서 온라인 상태인 CPU 수
- 현재 process의 cpuset/cgroup이 허용한 CPU 수
- 온도와 CPU hotplug 상태
- big/mid/LITTLE core 구성

Worker 하나가 CPU 하나에서 계속 실행되는지는 온라인 CPU 수, 허용 CPU mask, Worker 수, scheduler 상태에 따라 달라집니다.

## 메모리 부하가 커지는 원인

| 부하를 키우는 조건 | 메모리 계층에서 발생하는 변화 |
|---|---|
| cache보다 큰 테스트 메모리 | 이전 cache line이 밀려나는 횟수 증가 |
| 여러 CopyThread 동시 실행 | CPU cluster와 NoC에 여러 메모리 요청이 동시에 발생 |
| 1 MiB 단위의 임의 block 선택 | 같은 block을 짧은 시간 안에 다시 사용할 가능성 감소 |
| block 내부 순차 접근 | Hardware prefetch와 연속 burst 요청 증가 |
| 원본 읽기와 대상 쓰기 | cache refill과 dirty line 생성 |
| checksum 계산 | 읽은 데이터 검사와 CPU 연산 부하 추가 |

이 조건이 겹치면 L1/L2/SLC cache miss, cache refill, dirty line write-back, NoC 데이터 이동, DMC 요청이 증가합니다. 테스트 메모리는 운영체제가 설정한 일반 cacheable 속성을 그대로 사용합니다.

<sub><em>Cache miss: 요청한 cache line이 현재 cache level에 존재하지 않아 하위 계층 조회가 필요한 상태입니다.</em></sub><br>
<sub><em>Refill: cache miss가 발생한 line을 하위 cache 또는 system memory에서 가져와 cache에 채우는 동작입니다.</em></sub><br>
<sub><em>Dirty write-back: CPU store로 수정된 cache line을 하위 cache 또는 system memory 방향으로 기록하는 동작입니다.</em></sub>

# 프로그램 개요와 전체 구성

## 목적

stressapptest는 userspace에서 memory 및 I/O transaction을 생성하고, 부하가 실행되는 동안 이동 데이터가 예상 pattern을 유지하는지 검증하는 correctness-under-load test다.

<sub><em>Transaction: worker가 block을 획득한 시점부터 read·write·verify 후 queue에 반환할 때까지의 처리 단위입니다.</em></sub><br>
<sub><em>Pattern: memory block에 기록되는 반복 데이터 배열과 해당 배열의 기대 checksum 정보를 의미합니다.</em></sub>

목적별 처리 우선순위는 다음과 같다.

| 도구 유형 | 우선 처리 항목 |
|---|---|
| bandwidth benchmark | 단위 시간당 데이터 전송량 측정 |
| stressapptest | 부하 실행 중 source data의 무결성 검증 |

stressapptest의 처리 시간에는 checksum 계산, queue lock, random block 선택 및 scheduler yield가 포함된다. SAT throughput은 이 소프트웨어 처리 비용을 포함한 논리적 전송률로 정의된다.

<sub><em>Throughput: worker가 단위 시간에 처리했다고 계산한 논리적 데이터 양입니다.</em></sub><br>
<sub><em>Scheduler yield: 실행 중인 thread가 CPU 실행 기회를 scheduler에 자발적으로 반환하는 동작입니다.</em></sub>

## 상위 object 구성

```text
main()
 └─ Sat
     ├─ OsLayer
     │   ├─ memory allocation
     │   ├─ CPU/affinity 정보
     │   ├─ virtual→physical 진단
     │   ├─ cache maintenance abstraction
     │   └─ error polling abstraction
     ├─ PatternList
     │   └─ Pattern variants + expected checksum
     ├─ FineLockPEQueue 또는 PageEntryQueue
     │   └─ page_entry[각 SAT block]
     ├─ WorkerStatus
     │   ├─ power_spike_status
     │   └─ continuous_status
     └─ WorkerThread 파생 class들
```

`main()`의 순서는 `ParseArgs → Initialize → Run → PrintResults → Cleanup`이다 (`src/main.cc:20`).

## 핵심 데이터 흐름

```text
                    random Pattern 선택
                            ↓
empty block ──FillThread──→ valid block
    ↑                          │
    │                          │ source
    │                          ↓
    └────── CopyThread ─── destination
              read + checksum + write
```

CopyThread는 valid source와 empty destination을 각각 하나씩 잠근다. Copy 후:

- destination은 source와 동일한 pattern metadata를 가진 valid block이 된다.
- 기존 source는 empty block이 된다.

따라서 데이터 pattern은 memory pool 안에서 위치를 계속 바꾼다.

## 중요한 크기 단위

| 단위 | 기본값/예 | 의미 |
|---|---:|---|
| 64 B | `kCacheLineSize` | 코드가 가정하는 cache line 및 tag 간격 |
| 512 B | disk sector tag | FileThread/DiskThread의 sector 단위 |
| 4 KiB | checksum slice | expected checksum을 계산하고 비교하는 내부 단위 |
| 1 MiB | `kSatPageSize` | queue가 관리하는 기본 SAT block |
| `-M` MiB | 전체 working set | mmap하는 test memory 크기 |

1 MiB block 하나는 64 B cache line 16,384개, 4 KiB checksum slice 256개로 구성된다.

## 기본 thread 구성

기본 설정의 phase별 thread는 다음과 같다.

| phase | 기본 동작 |
|---|---|
| 초기 fill | FillThread 8개가 모든 block에 pattern write |
| block 분류 | 약 2/5를 empty, 약 3/5를 valid로 표시 |
| timed run | online logical CPU 수만큼 CopyThread |
| error monitor | ErrorPollThread 1개, generic ARM build에서는 실질적으로 no-op |
| final verify | CheckThread 8개가 남은 valid block 전체 검사 |

초기 fill과 final verify는 `-s` countdown의 측정 범위 밖에서 수행된다. 전체 process 실행 시간은 초기 fill 시간, `-s` 실행 시간 및 final verify 시간의 합으로 결정된다.

## CPU 수와 affinity

`OsLayer::Initialize()`는 `_SC_NPROCESSORS_ONLN`을 읽고, `-m`이 없으면 그 수를 CopyThread 수로 사용한다 (`src/os.cc:108`, `src/sat.cc:148`).

`memory_threads + cpu_stress_threads`가 사용 가능한 CPU 수 이하이면 thread를 교차된 CPU 번호에 pin하려 한다. 그렇지 않으면 별도 pinning을 하지 않고 scheduler가 여러 worker를 time-slice한다.

Android에서는 다음을 구분해야 한다.

- system online CPU 수
- process cpuset/cgroup이 허용한 CPU 수
- 현재 thermal/hotplug 상태
- 실제 big/mid/little microarchitecture

worker와 CPU의 일대일 대응 여부는 online CPU 수, 허용 CPU mask, worker 수 및 scheduler 상태에 따라 결정된다.

## Memory stress 구성 요소

| 구성 요소 | 실행 효과 |
|---|---|
| cache capacity보다 큰 working set | 이전에 접근한 cache line의 capacity eviction 증가 |
| 여러 CopyThread | CPU cluster와 NoC에 여러 memory transaction 동시 발행 |
| pseudo-random 1 MiB source/destination | block 간 temporal locality 감소 |
| block 내부 순차 접근 | hardware prefetch 및 연속 burst transaction 유도 |
| source read와 destination store | cache refill과 dirty line 생성 |
| checksum 연산 | load data 검증과 CPU execution resource 사용 |

이 접근 구조는 L1/L2/SLC miss, refill, dirty write-back, NoC traffic 및 DMC request를 증가시킨다. 테스트 메모리의 cache attribute는 운영체제가 설정한 normal cacheable 속성을 유지한다.

<sub><em>Cache miss: 요청한 cache line이 현재 cache level에 존재하지 않아 하위 계층 조회가 필요한 상태입니다.</em></sub><br>
<sub><em>Refill: cache miss가 발생한 line을 하위 cache 또는 system memory에서 가져와 cache에 채우는 동작입니다.</em></sub><br>
<sub><em>Dirty write-back: CPU store로 수정된 cache line을 하위 cache 또는 system memory 방향으로 기록하는 동작입니다.</em></sub>

# 프로그램 개요와 전체 구성

## 목적

stressapptest는 userspace에서 많은 memory 및 I/O transaction을 만들고, 이동하는 데이터가 예상 pattern을 유지하는지 지속적으로 확인하는 correctness-under-load test다.

최대 DRAM bandwidth만 얻는 benchmark와는 목적이 다르다.

- bandwidth benchmark: 가능한 많은 byte/s를 이동하는 것이 우선
- stressapptest: 부하가 있는 동안 source data가 손상되는지 검증하는 것이 우선

따라서 checksum 계산, queue lock, random block 선택, scheduler yield가 포함된다. 이 overhead 때문에 SAT가 표시하는 throughput이 SoC의 peak LPDDR bandwidth와 같지 않다.

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

초기 fill과 final verify는 `-s` countdown 밖에서 수행된다. 따라서 `-s 60`이라고 전체 process가 정확히 60초만 메모리를 사용하는 것이 아니다.

## CPU 수와 affinity

`OsLayer::Initialize()`는 `_SC_NPROCESSORS_ONLN`을 읽고, `-m`이 없으면 그 수를 CopyThread 수로 사용한다 (`src/os.cc:108`, `src/sat.cc:148`).

`memory_threads + cpu_stress_threads`가 사용 가능한 CPU 수 이하이면 thread를 교차된 CPU 번호에 pin하려 한다. 그렇지 않으면 별도 pinning을 하지 않고 scheduler가 여러 worker를 time-slice한다.

Android에서는 다음을 구분해야 한다.

- system online CPU 수
- process cpuset/cgroup이 허용한 CPU 수
- 현재 thermal/hotplug 상태
- 실제 big/mid/little microarchitecture

따라서 “8-core니까 항상 worker 하나가 core 하나를 전담한다”는 보장은 없다.

## stress가 만들어지는 이유

```text
큰 working set
  × 여러 worker
  × random 1 MiB source/destination
  × block 내부 순차 streaming
  × source read + destination dirty store
  × checksum 연산
= cache가 장시간 보관하기 어려운 다중 memory stream
```

이것이 L1/L2/SLC miss, refill, dirty write-back, NoC traffic, DMC request를 증가시킨다. cache를 강제로 disable하는 방식이 아니다.

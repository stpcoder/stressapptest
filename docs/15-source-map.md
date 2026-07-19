# 소스 코드 찾아보기

이 표는 기준 commit `73b9df2`에서 원하는 동작의 코드를 빠르게 찾을 수 있도록 파일과 시작 위치를 정리한 것입니다.

## 프로그램 시작과 종료

| 위치 | 내용 |
|---|---|
| `src/main.cc:20` | 옵션 확인 → 초기화 → 실행 → 결과 출력 → 자원 정리 |
| `src/sat.cc:661` | 모든 기본값 |
| `src/sat.cc:794` | 실제 명령행 옵션 처리 |
| `src/sat.cc:1095` | 기본 도움말. 실제 옵션 처리 코드와 일부 불일치 |
| `src/sat.cc:565` | 전체 초기화 |
| `src/sat.cc:1884` | 본 시험 실행, 일시 정지, 재시작, 종료 |
| `src/sat.cc:1533` | Worker 종료 대기와 마지막 전체 검사 |
| `src/sat.cc:1646` | 논리적 bandwidth 통계 |

## 메모리 할당과 주소 변환

| 위치 | 내용 |
|---|---|
| `src/os.cc:411` | 자동 테스트 메모리 크기 계산 |
| `src/os.cc:508` | hugepage/shm/mmap/memalign 선택 |
| `src/os.cc:641` | anonymous private mmap |
| `src/os.cc:141` | `/proc/self/pagemap` virtual→physical |
| `src/os.cc:285` | 공통 PA→DIMM·channel 추정 |
| `src/os.cc:313` | 공통 메모리 구역 분류 |
| `src/sat.cc:323` | `--do_page_map` 4 KiB bitmap |

## 메모리 block 관리

| 위치 | 내용 |
|---|---|
| `src/sattypes.h:245` | 1 MiB block, 64 B line, port 상수 |
| `src/queue.h:38` | `page_entry` 상태 정보 |
| `src/sat.cc:401` | 모든 block에 초기 데이터 쓰기와 valid·empty 상태 설정 |
| `src/sat.cc:248` | GetValid/GetEmpty/Put 상태 전이 |
| `src/finelock_queue.cc:24` | Block별 mutex를 사용하는 배열 구조 설명 |
| `src/finelock_queue.cc:327` | Pseudo-random 계산과 상태·tag 조건으로 block 선택 |
| `src/finelock_queue.cc:416` | PutEmpty/PutValid |
| `src/queue.cc:69` | Queue 전체를 잠그는 방식의 임의 block 선택 |

## 데이터 pattern

| 위치 | 내용 |
|---|---|
| `src/pattern.cc:26` | 15개 기본 pattern |
| `src/pattern.cc:246` | 4 KiB 기대 checksum 계산 |
| `src/pattern.cc:284` | 반복 범위와 반전 pattern 초기화 |
| `src/pattern.cc:338` | 120개 pattern 생성과 선택 비율 합산 |
| `src/pattern.cc:404` | 선택 비율에 따른 pattern 선택 |
| `src/pattern.h:59` | 반복 범위에 따른 word 반복 구현 |

## 메모리 Worker

| 위치 | 내용 |
|---|---|
| `src/worker.cc:475` | FillPage의 1 MiB 데이터 쓰기와 주소 tag 기록 |
| `src/worker.cc:524` | 임의 pattern 선택 |
| `src/worker.cc:1478` | `CheckThread` 반복문 |
| `src/worker.cc:1524` | `CopyThread` 반복문과 세 가지 복사 방식 |
| `src/worker.cc:1591` | `InvertThread`의 네 단계 반전 순서 |
| `src/worker.cc:1320` | 높은 주소에서 낮은 주소 방향의 RMW와 cache 관리 요청 |
| `src/worker.cc:1345` | 낮은 주소에서 높은 주소 방향의 RMW와 cache 관리 요청 |

## 메모리 복사와 오류 검사

| 위치 | 내용 |
|---|---|
| `src/adler32memcpy.cc:17` | modified Adler C 구현 |
| `src/adler32memcpy.cc:123` | Checksum과 복사를 함께 수행하는 C 코드 |
| `src/adler32memcpy.cc:163` | `-W`의 C 대체 코드 |
| `src/adler32memcpy.cc:402` | ARM·AArch64 vector 코드 |
| `src/worker.cc:720` | Word 단위 상세 비교 |
| `src/worker.cc:907` | 4 KiB CrcCheckPage |
| `src/worker.cc:1214` | 4 KiB `CrcCopyPage`와 일시적인 오류 재검사 |
| `src/worker.cc:960` | 주소 tag와 오류 처리 시작 |
| `src/os.cc:263` | 실행 중 `Flush()` 적용 조건 |
| `src/os.h:147` | architecture별 FastFlush |

## Worker 생성 위치

| 위치 | 내용 |
|---|---|
| `src/os.cc:108` | Online CPU 수 |
| `src/sat.cc:148` | 기본 CopyThread 수 |
| `src/sat.cc:1183` | 본 시험에서 사용할 모든 Worker 생성 |
| `src/sat.cc:1212` | CopyThread와 affinity |
| `src/sat.cc:1297` | CheckThread |
| `src/sat.cc:1308` | InvertThread |
| `src/sat.cc:1363` | CpuStressThread |
| `src/sat.cc:1399` | CPU별 cache coherency thread |
| `src/worker.cc:347` | pthread 생성 |
| `src/worker.cc:405` | 실행할 수 있는 CPU mask |

## I/O와 시스템 Worker

| 위치 | 내용 |
|---|---|
| `src/worker.cc:1649` | `FileThread`의 `O_DIRECT` 실패 후 대체 방식 |
| `src/worker.cc:1706` | 8개 block을 파일에 쓰기 |
| `src/worker.cc:1942` | 파일 읽기와 sector·checksum 검사 |
| `src/worker.cc:1989` | `FileThread` 작업 반복문 |
| `src/worker.cc:2062` | TCP socket/port |
| `src/worker.cc:2238` | 네트워크 송신·반환·수신·검사 |
| `src/worker.cc:2421` | 받은 데이터를 돌려보내는 네트워크 코드 |
| `src/worker.cc:2482` | ErrorPollThread |
| `src/worker.cc:2497` | CpuStressThread |
| `src/os.cc:904` | 공통 부동소수점 CPU 연산 |
| `src/worker.cc:2535` | Cache coherency counter 반복문 |
| `src/worker.cc:2622` | `DiskThread` 기본값 |
| `src/worker.cc:2866` | 저장 장치 쓰기·읽기 단계 |
| `src/worker.cc:3288` | RandomDiskThread |
| `src/worker.cc:3397` | 공개 명령 옵션에서 생성하지 않는 `MemoryRegionThread` |
| `src/worker.cc:3558` | x86 CPU frequency thread |

## 실행 환경별 구현 선택

| 위치 | 내용 |
|---|---|
| `src/os_factory.cc:30` | 항상 공통 `OsLayer` 생성 |
| `src/sat_factory.cc:19` | 공통 `Sat` 생성 |
| `src/os.cc:124` | 공개 build 지원 여부 판정 |
| `src/os.cc:739` | 실제 오류를 수집하지 않는 공통 `ErrorPoll()` |
| `src/os.cc:194` | ARM64 vector feature 가정 |
| `src/os.h:273` | AArch64 `CNTVCT_EL0` timestamp |

## 기능별 코드 확인 순서

> **파일:** `src/main.cc`, `src/sat.cc`, `src/worker.cc` · **기준:** `73b9df2`

```cpp
if (!sat->ParseArgs(argc, argv)) {
  logprintf(0, "Process Error: Sat::ParseArgs() failed\n");
  sat->bad_status();
} else if (!sat->Initialize()) {
  logprintf(0, "Process Error: Sat::Initialize() failed\n");
  sat->bad_status();
} else if (!sat->Run()) {
  logprintf(0, "Process Error: Sat::Run() failed\n");
  sat->bad_status();
}
sat->PrintResults();
```

**코드 설명:** 프로그램 시작 지점에서 확인할 세 단계는 옵션 확인, 초기화, 본 시험 실행입니다. 어느 단계에서 실패하더라도 프로그램 상태에 실패를 기록하고 결과를 출력합니다.

```text
main()
 ├─ Sat::ParseArgs()
 ├─ Sat::Initialize()
 │   ├─ OsLayerFactory() → OsLayer::Initialize()
 │   ├─ Sat::AllocateMemory()
 │   ├─ Sat::InitializePatterns()
 │   └─ Sat::InitializePages() → FillThread::Work()
 └─ Sat::Run()
     ├─ Sat::InitializeThreads()
     ├─ Sat::SpawnThreads() → WorkerThread::SpawnThread()
     ├─ CopyThread::Work() / CheckThread::Work() / InvertThread::Work()
     └─ Sat::JoinThreads() → final CheckThread
```

**코드 설명:** 위 호출 순서는 소스 코드를 확인하는 기본 경로입니다. 메모리 접근 과정을 분석할 때에는 `CopyThread::Work()`에서 선택된 조건을 확인한 뒤 `CrcCopyPage()`, `CrcWarmCopyPage()`, C library의 `memcpy()` 중 실제 호출된 함수로 이동합니다. 주소와 cache 관련 코드는 `OsLayer`와 `adler32memcpy.cc`에서 확인합니다.

메모리 복사 코드는 다음 순서로 확인합니다.

```text
Sat::InitializePages
 → FillThread::Work / FillPage
 → Sat::InitializeThreads
 → CopyThread::Work
 → WorkerThread::CrcCopyPage
 → AdlerMemcpyC 또는 AdlerMemcpyAsm
 → CheckRegion / ProcessError
 → Sat::JoinThreads final CheckThread
```

Physical address와 cache 관련 코드는 다음 순서로 확인합니다.

```text
OsLayer::AllocateTestMem
 → Sat::GetValid/GetEmpty
 → OsLayer::VirtualToPhysical
 → OsLayer::GetFeatures
 → OsLayer::AdlerMemcpyWarm
 → OsLayer::FastFlush/Flush
```

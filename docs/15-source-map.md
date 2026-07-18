# 소스 코드 지도

이 표는 기준 commit `73b9df2`에서 분석을 다시 시작할 때 사용할 entry point다.

## Program lifecycle

| 위치 | 내용 |
|---|---|
| `src/main.cc:20` | ParseArgs → Initialize → Run → PrintResults → Cleanup |
| `src/sat.cc:661` | 모든 기본값 |
| `src/sat.cc:794` | 실제 argument parser |
| `src/sat.cc:1095` | built-in help, parser와 일부 불일치 |
| `src/sat.cc:565` | 전체 initialization |
| `src/sat.cc:1884` | timed run, pause/resume, stop |
| `src/sat.cc:1533` | worker join 및 final check |
| `src/sat.cc:1646` | logical bandwidth 통계 |

## Memory allocation 및 address

| 위치 | 내용 |
|---|---|
| `src/os.cc:411` | 자동 memory 크기 계산 |
| `src/os.cc:508` | hugepage/shm/mmap/memalign 선택 |
| `src/os.cc:641` | anonymous private mmap |
| `src/os.cc:141` | `/proc/self/pagemap` virtual→physical |
| `src/os.cc:285` | generic PA→DIMM/channel 추정 |
| `src/os.cc:313` | generic region 분류 |
| `src/sat.cc:323` | `--do_page_map` 4 KiB bitmap |

## Block과 queue

| 위치 | 내용 |
|---|---|
| `src/sattypes.h:245` | 1 MiB block, 64 B line, port 상수 |
| `src/queue.h:38` | `page_entry` metadata |
| `src/sat.cc:401` | 모든 block fill 및 valid/empty 구성 |
| `src/sat.cc:248` | GetValid/GetEmpty/Put 상태 전이 |
| `src/finelock_queue.cc:24` | fine-lock array 설계 설명 |
| `src/finelock_queue.cc:327` | pseudo-random predicate/tag block 선택 |
| `src/finelock_queue.cc:416` | PutEmpty/PutValid |
| `src/queue.cc:69` | coarse queue random pop |

## Pattern

| 위치 | 내용 |
|---|---|
| `src/pattern.cc:26` | 15개 static pattern family |
| `src/pattern.cc:246` | 4 KiB expected checksum 계산 |
| `src/pattern.cc:284` | width/inverse variant 초기화 |
| `src/pattern.cc:338` | 120 variant 생성 및 weight 합산 |
| `src/pattern.cc:404` | weighted random pattern 선택 |
| `src/pattern.h:59` | logical width의 word 반복 구현 |

## Memory worker

| 위치 | 내용 |
|---|---|
| `src/worker.cc:475` | FillPage 1 MiB word store/tag |
| `src/worker.cc:524` | random pattern 선택 |
| `src/worker.cc:1478` | CheckThread loop |
| `src/worker.cc:1524` | CopyThread loop 및 3개 copy mode |
| `src/worker.cc:1591` | InvertThread four-pass sequence |
| `src/worker.cc:1320` | downward RMW 및 flush hint |
| `src/worker.cc:1345` | upward RMW 및 flush hint |

## Copy/check/error

| 위치 | 내용 |
|---|---|
| `src/adler32memcpy.cc:17` | modified Adler C 구현 |
| `src/adler32memcpy.cc:123` | fused checksum+copy C path |
| `src/adler32memcpy.cc:163` | warm C path |
| `src/adler32memcpy.cc:402` | ARM/AArch64 vector path |
| `src/worker.cc:720` | word-by-word slow compare |
| `src/worker.cc:907` | 4 KiB CrcCheckPage |
| `src/worker.cc:1214` | 4 KiB CrcCopyPage 및 transient retry |
| `src/worker.cc:960` | tag/error processing 시작 |
| `src/os.cc:263` | runtime Flush gate |
| `src/os.h:147` | architecture별 FastFlush |

## Worker creation

| 위치 | 내용 |
|---|---|
| `src/os.cc:108` | online CPU count |
| `src/sat.cc:148` | 기본 CopyThread 수 |
| `src/sat.cc:1183` | 모든 timed worker 생성 |
| `src/sat.cc:1212` | CopyThread와 affinity |
| `src/sat.cc:1297` | CheckThread |
| `src/sat.cc:1308` | InvertThread |
| `src/sat.cc:1363` | CpuStressThread |
| `src/sat.cc:1399` | per-CPU cache coherency thread |
| `src/worker.cc:347` | pthread creation |
| `src/worker.cc:405` | available CPU mask |

## I/O 및 system worker

| 위치 | 내용 |
|---|---|
| `src/worker.cc:1649` | FileThread O_DIRECT fallback |
| `src/worker.cc:1706` | 8-block file write |
| `src/worker.cc:1942` | file read/sector/checksum 검증 |
| `src/worker.cc:1989` | FileThread pass loop |
| `src/worker.cc:2062` | TCP socket/port |
| `src/worker.cc:2238` | network send/reflect/receive/check |
| `src/worker.cc:2421` | network reflector |
| `src/worker.cc:2482` | ErrorPollThread |
| `src/worker.cc:2497` | CpuStressThread |
| `src/os.cc:904` | generic FP CPU workload |
| `src/worker.cc:2535` | cache coherency counter loop |
| `src/worker.cc:2622` | DiskThread defaults |
| `src/worker.cc:2866` | disk write/read phase |
| `src/worker.cc:3288` | RandomDiskThread |
| `src/worker.cc:3397` | public CLI에서 미사용인 MemoryRegionThread |
| `src/worker.cc:3558` | x86 CPU frequency thread |

## Factory 및 generic 한계

| 위치 | 내용 |
|---|---|
| `src/os_factory.cc:30` | 항상 generic `OsLayer` 생성 |
| `src/sat_factory.cc:19` | generic `Sat` 생성 |
| `src/os.cc:124` | open-source build supported 판정 |
| `src/os.cc:739` | generic ErrorPoll no-op |
| `src/os.cc:194` | ARM64 vector feature 가정 |
| `src/os.h:273` | AArch64 `CNTVCT_EL0` timestamp |

## 분석 시 권장 call chain

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

**해석:** 실제 entry point에서 분석할 세 단계는 argument parse, initialization, timed run입니다. 각 단계의 실패는 process status에 반영되며 결과 출력까지 수행됩니다.

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

**해석:** 위 call chain은 source browsing의 기준 경로입니다. memory access 원인을 분석할 때는 `CopyThread::Work()`에서 선택된 branch를 확인한 다음 `CrcCopyPage()`, `CrcWarmCopyPage()` 또는 libc `memcpy()`로 이동합니다. address와 cache 동작은 해당 함수가 호출하는 `OsLayer` 및 `adler32memcpy.cc`에서 확인합니다.

Copy data path는 다음 순서로 읽는다.

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

Physical/cache path는 다음 순서로 분석한다.

```text
OsLayer::AllocateTestMem
 → Sat::GetValid/GetEmpty
 → OsLayer::VirtualToPhysical
 → OsLayer::GetFeatures
 → OsLayer::AdlerMemcpyWarm
 → OsLayer::FastFlush/Flush
```

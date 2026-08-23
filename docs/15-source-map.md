# 소스 코드 찾아보기

소스 위치는 파일명과 함수명으로 표시합니다. 함수명은 줄 번호가 변경되어도 유지되므로 저장소 검색에 사용할 수 있습니다.

```bash
rg -n 'Sat::Run|CopyThread::Work|InvertThread::Work' src
```

## 프로그램 시작과 종료

| 파일과 함수 | 기능 |
|---|---|
| `src/main.cc`의 `main()` | 옵션 확인, 초기화, 실행, 결과 출력과 자원 정리 |
| `src/sat.cc`의 `Sat::Sat()` | 기본 옵션값 설정 |
| `src/sat.cc`의 `Sat::ParseArgs()` | 명령행 옵션 해석과 입력값 검사 |
| `src/sat.cc`의 `Sat::PrintHelp()` | 명령행 도움말 출력 |
| `src/sat.cc`의 `Sat::Initialize()` | 운영체제 계층, 메모리, Pattern과 queue 초기화 |
| `src/sat.cc`의 `Sat::Run()` | Runtime Worker 생성, 실행 시간 제어와 종료 |
| `src/sat.cc`의 `Sat::JoinThreads()` | Runtime Worker 회수와 종료 시점 Valid 검사 |
| `src/sat.cc`의 `Sat::RunAnalysis()` | Worker별 처리량과 오류 수 집계 |

## 메모리 준비와 Fill

| 파일과 함수 | 기능 |
|---|---|
| `src/os.cc`의 `OsLayer::FindFreeMemSize()` | 사용 가능한 시험 메모리 크기 계산 |
| `src/os.cc`의 `OsLayer::AllocateTestMem()` | hugepage, shared memory와 anonymous mapping 선택 |
| `src/sat.cc`의 `Sat::InitializePages()` | 선택형 사전 접근, Fill, post-fill 검사와 queue 구성 |
| `src/sat.cc`의 `Sat::RunFillPass()` | 사전 채움과 최종 Pattern Fill Worker 실행 |
| `src/worker.cc`의 `FillThread::Work()` | Empty 작업 단위 선택, Fill과 선택형 즉시 검사 |
| `src/worker.cc`의 `FillThread::FillPageRandom()` | 작업 단위에 적용할 Pattern 선택 |
| `src/worker.cc`의 `WorkerThread::FillPage()` | SAT 작업 단위에 Pattern 저장 |
| `src/worker.cc`의 `WorkerThread::FillPageWithConstant()` | 사전 채움용 동일값 저장 |
| `src/worker.cc`의 `PostFillCheckThread::Work()` | Runtime queue 구성 전 작업 단위 검사 |

## SAT 작업 단위와 queue

| 파일과 함수 | 기능 |
|---|---|
| `src/sattypes.h`의 `kSatPageSize`, `kCacheLineSize` | 기본 1 MiB 작업 단위와 64 B cache line 상수 |
| `src/queue.h`의 `page_entry` | 주소, Pattern, 상태와 최근 write 정보를 보관하는 구조체 |
| `src/sat.cc`의 `Sat::GetValid()`, `Sat::GetEmpty()` | Worker가 사용할 Valid 또는 Empty 작업 단위 획득 |
| `src/sat.cc`의 `Sat::PutValid()`, `Sat::PutEmpty()` | 처리한 작업 단위의 queue 상태 갱신 |
| `src/sat.cc`의 `Sat::GetValidByOffsetForInitialization()` | FineLock post-fill 검사의 논리 offset 조회 |
| `src/finelock_queue.cc`의 `FineLockPEQueue::GetRandomWithPredicateTag()` | FineLock queue의 작업 단위 선택 |
| `src/finelock_queue.cc`의 `FineLockPEQueue::PutValid()`, `PutEmpty()` | 작업 단위별 lock을 사용한 상태 반환 |
| `src/queue.cc`의 `PageEntryQueue` | 전체 queue lock을 사용하는 선택형 구현 |

## Pattern

| 파일과 함수 | 기능 |
|---|---|
| `src/pattern.cc`의 `pattern_array` | 기본 Pattern 데이터와 선택 가중치 |
| `src/pattern.cc`의 `Pattern::Initialize()` | Pattern 폭, 반전 상태와 byte offset 설정 |
| `src/pattern.cc`의 `Pattern::CalculateCrc()` | 4 KiB expected checksum 계산 |
| `src/pattern.cc`의 `PatternList::Initialize()` | 실행 중 사용할 Pattern 객체 구성 |
| `src/pattern.cc`의 `PatternList::SetPatternSequence()` | `-P`의 ID·이름 목록 확인 |
| `src/pattern.cc`의 `PatternList::GetRandomPattern()` | 지정 목록 순환 또는 가중치 기반 Pattern 선택 |
| `src/pattern.h`의 `Pattern::pattern()` | 주소별 32-bit expected 값 계산 |

## Runtime 메모리 Worker

| 파일과 함수 | 기능 |
|---|---|
| `src/sat.cc`의 `Sat::InitializeThreads()` | `-m`, `-i`, `-c`와 다른 Runtime Worker 객체 구성 |
| `src/sat.cc`의 `Sat::SpawnThreads()` | Worker pthread 생성과 생성 실패 처리 |
| `src/worker.cc`의 `WorkerThread::SpawnThread()` | `pthread_create()` 호출과 생성 상태 기록 |
| `src/worker.cc`의 `WorkerThread::JoinThread()` | 생성에 성공한 pthread 회수 |
| `src/worker.cc`의 `CopyThread::Work()` | Valid source와 Empty destination 선택, 복사와 queue 교환 |
| `src/worker.cc`의 `CheckThread::Work()` | Runtime 반복 검사 또는 종료 시점 Valid 검사 |
| `src/worker.cc`의 `PostFillCheckThread::Work()` | Runtime queue 구성 전 전체 Fill 결과 검사 |
| `src/worker.cc`의 `InvertThread::Work()` | 반전 전 검사, 네 번의 반전 저장과 반전 후 검사 |
| `src/worker.cc`의 `InvertThread::InvertPageUp()` | 낮은 주소에서 높은 주소 방향의 read-modify-write |
| `src/worker.cc`의 `InvertThread::InvertPageDown()` | 높은 주소에서 낮은 주소 방향의 read-modify-write |
| `src/invert_workload.h`의 `InvertWordsUp()`, `InvertWordsDown()` | 생산 코드와 단위 테스트가 공유하는 방향별 반전 루프 |

## 복사와 오류 검사

| 파일과 함수 | 기능 |
|---|---|
| `src/adler32memcpy.cc`의 `AdlerMemcpyC()` | Source read, checksum 계산과 destination write |
| `src/adler32memcpy.cc`의 `AdlerMemcpyAsm()` | ARM·AArch64 벡터 복사 경로 |
| `src/worker.cc`의 `WorkerThread::CrcCopyPage()` | 4 KiB 단위 source checksum과 복사 |
| `src/worker.cc`의 `WorkerThread::CrcCheckPage()` | 4 KiB 단위 checksum 검사 |
| `src/worker.cc`의 `WorkerThread::CheckRegion()` | Checksum mismatch 구간의 64-bit 상세 비교 |
| `src/worker.cc`의 `WorkerThread::ProcessError()` | Reread, 주소·단계 로그와 expected 값 복구 |
| `src/worker.cc`의 `WorkerThread::RecordDiagnosticOperation()` | Worker local 단계별 논리 작업량 집계 |
| `src/os.cc`의 `OsLayer::Flush()` | 아키텍처 기능 상태에 따른 상세 검사 전 cache 관리 요청 |
| `src/os.h`의 `FastFlush()`, `FastFlushHint()` | 아키텍처별 cache 관리 명령 구현 |

## 주소와 DRAM 주파수 정보

| 파일과 함수 | 기능 |
|---|---|
| `src/os.cc`의 `OsLayer::VirtualToPhysical()` | `/proc/self/pagemap` 기반 가상 주소-물리 주소 변환 시도 |
| `src/sat.cc`의 `Sat::AddrMapInit()`, `AddrMapUpdate()`, `AddrMapPrint()` | 시험 메모리의 물리 구역 정보 구성 |
| `src/dram_address.h`의 `DecodeDramAddress()` | 명시적으로 선택한 주소 해석 프로필 적용 |
| `src/sat.cc`의 `Sat::ApplyDramFrequency()` | DDR 제어 경로에 요청값 전달과 로그 기록 |
| `src/sat.cc`의 `Sat::RecordBlockWrite()` | 선택형 추적 write 작업의 완료 이력 갱신 |
| `src/sat.cc`의 `Sat::LogVmStats()` | 단계 경계의 page fault, RSS와 mapping 정보 출력 |
| `src/sat.cc`의 `Sat::PrintDiagnosticPhaseSummary()` | 단계별 논리 작업량과 mismatch 수 출력 |
| `src/worker.cc`의 `FormatDramFrequencies()` | Write·read·reread 시점의 요청값 문자열 구성 |
| `src/worker.cc`의 `FormatDramCoordinates()` | 선택한 프로필의 주소 해석 결과 출력 |

## Logger

| 파일과 함수 | 기능 |
|---|---|
| `src/logger.cc`의 `Logger::VLogF()` | Timestamp와 로그 문자열 생성 |
| `src/logger.cc`의 `Logger::QueueLogLine()` | 공용 로그 queue에 메시지 저장 |
| `src/logger.cc`의 `Logger::ThreadMain()` | 전용 Logger thread의 queue 처리 |
| `src/logger.cc`의 `Logger::WriteAndDeleteLogLine()` | Logfile과 표준 출력에 메시지 기록 |
| `src/sat.cc`의 `Sat::InitializeLogfile()` | `-l` 파일 열기와 동기 쓰기 설정 |

## I/O와 시스템 Worker

| 파일과 함수 | 기능 |
|---|---|
| `src/worker.cc`의 `FileThread::Work()` | 파일 write·read와 checksum 검사 |
| `src/worker.cc`의 `NetworkThread::Work()` | 네트워크 송신·수신과 데이터 검사 |
| `src/worker.cc`의 `NetworkSlaveThread::Work()` | 수신 데이터를 송신 측에 반환 |
| `src/worker.cc`의 `ErrorPollThread::Work()` | 운영체제 계층의 오류 polling 호출 |
| `src/worker.cc`의 `CpuStressThread::Work()` | 부동소수점 CPU workload 반복 |
| `src/worker.cc`의 `CpuCacheCoherencyThread::Work()` | 공유 cache line counter 접근 |
| `src/worker.cc`의 `DiskThread::Work()` | 저장 장치 write·read 시험 |
| `src/worker.cc`의 `RandomDiskThread::DoWork()` | 저장 장치 임의 위치 read 시험 |

## 코드 확인 순서

```text
main
 → Sat::ParseArgs
 → Sat::Initialize
 → Sat::InitializePages
 → Sat::RunFillPass
 → FillThread::Work
 → WorkerThread::FillPage
 → Sat::Run
 → Sat::InitializeThreads
 → Sat::SpawnThreads
 → CopyThread::Work / CheckThread::Work / InvertThread::Work
 → CrcCopyPage / CrcCheckPage
 → CheckRegion / ProcessError
 → Sat::JoinThreads
```

Copy 경로는 다음 순서로 확인합니다.

```text
CopyThread::Work
 → Sat::GetValid / Sat::GetEmpty
 → WorkerThread::CrcCopyPage
 → AdlerMemcpyC 또는 AdlerMemcpyAsm
 → WorkerThread::CheckRegion
 → WorkerThread::ProcessError
 → Sat::PutValid / Sat::PutEmpty
```

Invert 경로는 다음 순서로 확인합니다.

```text
InvertThread::Work
 → CrcCheckPage(precheck)
 → InvertPageUp
 → InvertPageDown
 → InvertPageDown
 → InvertPageUp
 → CrcCheckPage(postcheck)
```

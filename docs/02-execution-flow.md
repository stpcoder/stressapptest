# 실행 순서 한눈에 보기

## 작동 단계별 순서

```text
명령행 옵션 확인
   ↓
OS/CPU 정보 초기화
   ↓
테스트 메모리 크기 결정과 mmap
   ↓
Pattern과 기대 checksum 생성
   ↓
1 MiB block 정보와 queue 생성
   ↓
8개 FillThread가 전체 메모리에 쓰기       ┐
   ↓                                      │ -s 시작 전
물리 주소 확인과 block 상태 설정          ┘
   ↓
Worker 생성
   ↓
설정한 시간 동안 복사·검사·반전·I/O 실행
   ↓
Worker 정지와 종료 대기
   ↓
8개 CheckThread로 종료 시점에 남은 valid block 검사  ← 실행 시간 종료 후
   ↓
통계와 오류를 출력한 뒤 메모리 해제
```

## 1. 실행 옵션 확인

`Sat::ParseArgs()`는 사용자가 입력한 옵션을 앞에서부터 확인하고 각 옵션 이름을 문자열로 비교합니다. 구현은 `src/sat.cc`의 `Sat::ParseArgs()`에 있습니다.

옵션은 다음 규칙으로 처리됩니다.

- 기존 정수 옵션 다수는 `strtoull(..., base=0)`를 사용하여 10진수와 `0x` 접두사의 16진수를 처리합니다.
- 추가 진단 옵션과 `--printsec`, `--pause_delay`는 전체 문자열과 값의 범위를 확인하는 10진수 parser를 사용합니다.
- 기존 매크로 경로의 일부 signed 변수에는 unsigned 방식으로 변환한 값을 저장합니다.
- `-f`, `-d`, `-n`, `--memory_channel`은 여러 번 지정할 수 있습니다.
- 옵션 목록 밖의 문자열을 입력하면 version과 도움말을 출력하고 종료 코드 1을 반환합니다.
- `-h` 또는 `--help`를 입력하면 version과 도움말을 출력하고 종료 코드 0을 반환합니다.

### 프로그램 초기화 순서

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

**코드 설명:** `OsLayer`를 초기화하면서 CPU 수와 메모리 기본값을 정합니다. 테스트 메모리 할당이 성공하면 pattern checksum과 SAT block 수를 준비합니다. 이 모든 과정과 초기 데이터 쓰기는 `-s`로 지정한 실행 시간이 시작되기 전에 수행됩니다.

## 2. CPU 정보와 기본 설정 확인

- 실행 시간 기본값: 20초
- SAT block 크기: 1 MiB
- 초기 쓰기 FillThread: 8개
- CopyThread: 미지정 시 온라인 상태의 논리 CPU 수
- 복사 중 checksum 검사: 사용
- `-W` vector 복사: 비활성
- 주기적 정지: 600초마다 15초

`-M`의 기본값은 `FindFreeMemSize()`가 전체 RAM을 기준으로 목표 시험 크기를 계산합니다. 현재 사용 가능한 메모리는 로그에 표시됩니다. Android 시험에서는 시스템 프로세스용 여유 공간을 고려한 `-M` 값을 직접 지정합니다.

## 3. 테스트 메모리 할당

Android/Linux의 일반 할당 경로에서는 다음 `mmap()`을 사용합니다.

```c
mmap(NULL, length,
     PROT_READ | PROT_WRITE,
     MAP_PRIVATE | MAP_ANONYMOUS,
     -1, 0)
```

`mmap()`이 성공하면 먼저 가상 주소 영역이 예약됩니다. 아직 연결되지 않은 물리 페이지는 FillThread가 해당 페이지에 처음 데이터를 쓸 때 page fault를 거쳐 할당됩니다.

<sub><em>Anonymous mmap: 파일 연결 없이 프로세스용 가상 주소 범위를 확보하는 Linux 메모리 매핑 방식입니다.</em></sub>
<sub><em>First-touch: 예약된 가상 페이지에 처음 접근하는 동작입니다. 아직 연결된 물리 페이지가 없으면 kernel이 이 시점에 물리 페이지를 할당합니다.</em></sub>

## 4. 테스트 데이터 pattern 준비

15개 pattern 종류마다 다음 조합을 만듭니다.

- 원본과 bit 반전 pattern
- 32/64/128/256-bit 반복 범위

하나의 pattern 종류에서 8개 조합이 만들어지므로 전체 pattern 객체는 120개입니다. 시험에서는 양수 가중치를 가진 객체를 선택합니다.

각 pattern 객체는 4 KiB 데이터에 대한 기대 modified-Adler checksum을 미리 계산합니다.

## 5. 전체 메모리에 초기 데이터 쓰기

처음에는 모든 `page_entry.pattern` 값이 null이므로 전체 block이 empty 상태입니다. FillThread 8개가 각 block을 가져와 다음 작업을 수행합니다.

1. 선택 가중치에 따라 pattern 하나 선택
2. 1 MiB block 전체에 64-bit 단위로 pattern 기록
3. 사용한 pattern과 마지막으로 쓴 CPU 번호 기록
4. block을 valid 상태로 변경

이 단계에서 전체 테스트 메모리에 Pattern을 기록하므로 쓰기 요청이 집중됩니다. 아직 물리 페이지가 연결되지 않은 주소에서는 물리 할당도 함께 발생합니다. 이 작업은 `-s` 실행 시간이 시작되기 전에 완료됩니다.

## 6. 물리 주소 확인과 block 상태 설정

모든 block에 데이터를 쓴 다음 각 block을 다시 확인하여 다음 정보를 설정합니다.

- block 첫 가상 주소에 대응하는 물리 주소를 `/proc/self/pagemap`으로 조회
- 공통 `OsLayer` 방식으로 region tag 계산
- `--do_page_map` 사용 시 4 KiB 단위 주소 bitmap 갱신
- 기본 fine-lock queue에서 약 2/5를 empty, 약 3/5를 valid로 설정

`empty`는 다음 복사의 대상 block으로 사용할 수 있다는 뜻입니다. 1 MiB 메모리와 물리 페이지는 계속 할당된 상태로 유지됩니다.

<sub><em>Valid block: 기대 pattern 정보를 보유하며 복사의 원본 또는 검사의 대상으로 사용할 수 있는 SAT block입니다.</em></sub>
<sub><em>Empty block: 새 데이터를 쓸 대상으로 사용할 수 있도록 pattern 정보가 해제된 SAT block입니다.</em></sub>

## 7. 설정한 시간 동안 Worker 실행

`Sat::Run()`은 Worker 객체를 준비하고 `pthread_create()`로 thread를 시작한 뒤 `-s` 실행 시간을 측정합니다. 구현은 `src/sat.cc`의 `Sat::Run()`에 있습니다.

> **파일:** `src/sat.cc` · **함수:** `Sat::Run()` · **기준:** `73b9df2`

```cpp
InitializeThreads();
SpawnThreads();

const time_t start = time(NULL);
const time_t end = start + runtime_seconds_;
```

**코드 설명:** `InitializeThreads()`는 C++ Worker 객체를 만들고, `SpawnThreads()`는 각 Worker를 실제 pthread로 실행합니다. `-s`는 pthread 실행 구간에 적용됩니다. 초기 FillThread는 이 구간 전에 실행되고 마지막 CheckThread는 이 구간 후에 실행됩니다.

기본 CopyThread는 다음 순서를 반복합니다.

```text
GetValid(원본)
GetEmpty(대상)
CrcCopyPage(원본 → 대상)
PutValid(대상)
PutEmpty(원본)
sched_yield()
```

## 8. Worker 일시 정지와 재시작

기본 설정은 `--pause_delay 600 --pause_duration 15`입니다. 실행 시작 후 600초가 지나면 `power_spike_status`에 속한 Worker를 15초 동안 멈춘 후 다시 실행합니다.

`power_spike_status`에는 주로 CopyThread, FileThread, DiskThread, CPU 주파수 확인 Worker가 들어갑니다. `continuous_status`의 Worker는 계속 실행됩니다. 기본 pause 주기는 600초이므로 더 짧은 시험은 연속 부하로 진행됩니다.

## 9. 종료 시점의 Valid 데이터 검사

설정 시간이 끝나면 모든 Worker에 정지 요청을 보냅니다. 기본 동작에서 Runtime `CheckThread`는 Valid queue를 계속 검사하고 완료한 block을 Empty로 이동합니다. 남은 Valid block은 `--fill-threads`와 같은 수의 보조 Check Worker가 검사합니다.

`--final-check-threads N`을 명시하면 Runtime Check Worker가 보유 block을 Valid로 반환하고 종료합니다. 이후 N개의 별도 Check Worker가 남은 Valid block을 검사합니다. `--skip-final-check`는 두 종료 검사 경로를 모두 생략합니다. `--stop_on_errors`로 종료한 실행도 종료 검사를 생략합니다.

대상 block에 데이터를 쓰는 과정에서 발생한 오류는 다음 검사 시점에 발견될 수 있습니다.

- 해당 block이 다음 복사의 원본으로 선택될 때
- 실행 중 CheckThread가 해당 block을 검사할 때
- 종료 시점의 Valid 검사에서

## 10. 프로그램 종료 코드

- 내부 실행 오류 또는 데이터 오류가 하나라도 있으면 종료 코드 1
- 오류 없이 정상 완료하면 종료 코드 0

재부팅, kernel panic, watchdog reset, SIGKILL은 Logger의 정상 종료 절차를 생략할 수 있습니다. 이 경우 pstore, kernel log와 LMKD log에서 종료 원인을 확인합니다.

# 오류 검사와 로그 처리 과정

이 장은 Memory Worker가 데이터 오류를 검출하고 로그를 출력하는 과정을 설명합니다. 설명 기준은 이 저장소의 Android AArch64 코드입니다.

## 오류 검사 과정

```text
Fill Worker가 pattern 기록
 → Worker가 block을 읽고 checksum 계산
 → checksum mismatch 검출
 → CheckRegion()이 64-bit 단위로 값 비교
 → ErrorRecord에 read와 expected 저장
 → ProcessError()가 같은 주소를 reread
 → 주소와 CPU 정보를 오류 메시지로 생성
 → Logger가 logfile과 stdout에 출력
 → 해당 주소를 expected 값으로 복구
```

`read`, `reread`, `expected`가 포함된 상세 로그는 64-bit 값의 mismatch를 검출할 때 생성됩니다.

## Worker 검사 기능

| Worker | 주요 함수 | 수행 기능 |
|---|---|---|
| Fill Worker | `FillPage()` | Empty block에 expected pattern을 기록합니다. |
| Copy Worker | `CrcCopyPage()` | Source를 destination으로 복사하면서 source checksum을 계산합니다. |
| Warm Copy Worker | `CrcWarmCopyPage()` | CPU 연산을 포함한 copy와 checksum 검사를 수행합니다. |
| Invert Worker | `InvertPageUp()`, `InvertPageDown()` | Block 값을 순방향과 역방향으로 반전하고 checksum을 검사합니다. |
| Check Worker | `CrcCheckPage()` | Block을 읽고 checksum과 expected pattern을 검사합니다. |

`page_entry`에는 block 주소, expected pattern과 마지막 writer CPU가 저장됩니다.

> **소스 위치:** `src/worker.cc`의 `FillPage()`, `CrcCopyPage()`, `CrcWarmCopyPage()`, `InvertThread::Work()`, `CrcCheckPage()`

## 1단계: checksum 검사

Strict copy와 Check Worker는 4 KiB 구간마다 modified Adler checksum을 계산합니다.

```text
4 KiB 데이터 읽기
 → checksum 계산
 → expected checksum과 비교
```

Checksum이 일치하면 다음 구간을 처리합니다. Mismatch가 발생하면 `CheckRegion()`이 해당 구간을 상세 검사합니다.

```cpp
if (!crc.Equals(*expectedcrc)) {
  CheckRegion(...);
}
```

Copy Worker는 source checksum을 계산하면서 destination에 같은 데이터를 기록합니다.

> **소스 위치:** `src/worker.cc`의 `CrcCheckPage()`, `CrcCopyPage()`

## 2단계: 64-bit 상세 검사

`CheckRegion()`은 checksum mismatch 구간을 64-bit 단위로 읽고 pattern의 expected 값과 비교합니다.

```text
read     = memory에서 읽은 64-bit 값
expected = pattern으로 계산한 64-bit 값
```

`read != expected`인 주소마다 다음 정보를 `ErrorRecord`에 저장합니다.

| 필드 | 저장 내용 |
|---|---|
| `actual` | `CheckRegion()`에서 읽은 값 |
| `expected` | Pattern으로 계산한 값 |
| `vaddr` | Mismatch가 발생한 virtual address |
| `patternname` | Expected 값을 만든 pattern 이름 |
| `lastcpu` | Block의 마지막 writer CPU |

한 번의 `CheckRegion()`은 최대 128개 record를 저장한 후 `ProcessError()`에 순서대로 전달합니다.

```cpp
const int kErrorLimit = 128;
struct ErrorRecord recorded[kErrorLimit];
```

> **소스 위치:** `src/worker.cc`의 `WorkerThread::CheckRegion()`

## 3단계: reread와 오류 메시지 생성

`ProcessError()`는 각 `ErrorRecord`를 다음 순서로 처리합니다.

```text
OsLayer::Flush(vaddr) 호출
 → 같은 virtual address load
 → physical address 변환 시도
 → 상세 오류 메시지 생성
 → expected 값을 해당 주소에 기록
```

오류 로그의 두 관찰값은 다음 함수에서 생성됩니다.

```text
read   = CheckRegion()의 상세 검사값
reread = ProcessError()의 두 번째 load 값
```

Expected 값 복구는 같은 손상값이 후속 copy 작업으로 전달되는 범위를 줄입니다. 최초 오류 분석에는 복구 전에 생성된 첫 상세 로그를 사용합니다.

> **소스 위치:** `src/worker.cc`의 `WorkerThread::ProcessError()`

### AArch64 `Flush()` 실행 조건

`OsLayer::Flush()`는 `has_clflush_ == true` 조건에서 `FastFlush()`를 실행합니다.

```cpp
void OsLayer::Flush(void *vaddr) {
  if (has_clflush_) {
    OsLayer::FastFlush(vaddr);
  }
}
```

공개 AArch64 경로의 `has_clflush_` 값은 `false`입니다. 이 조건에서 `Flush()`는 cache 관리 명령 없이 반환하고, `reread`는 같은 virtual address에 대한 두 번째 CPU load로 수행됩니다.

Invert Worker는 `FastFlushHint()` 경로에서 `DC CVAU`를 실행합니다. `DC CVAU`는 Data Cache line을 Point of Unification까지 clean하고 line의 valid 상태를 유지할 수 있습니다.

따라서 로그는 CPU가 두 시점에 관찰한 값을 기록합니다. 실제 응답 계층은 PMU, system cache monitor 또는 memory-controller counter로 확인합니다.

<sub><em>Clean: Dirty cache line의 값을 지정된 coherency 지점까지 기록하는 cache 관리 동작입니다.</em></sub>
<sub><em>Point of Unification: Instruction fetch와 data access가 같은 memory 값을 관찰하도록 합쳐지는 지점입니다.</em></sub>

> **소스 위치:** `src/os.cc`의 `OsLayer::GetFeatures()`, `OsLayer::Flush()`와 `src/os.h`의 `FastFlush()`, `FastFlushHint()`

## 4단계: Logger 출력

Worker의 `logprintf()`는 메시지를 공용 Logger queue에 전달합니다.

```text
Worker의 logprintf()
 → Logger::VLogF()
 → timestamp와 문자열 생성
 → queued_lines_에 저장
 → Logger thread가 queue 처리
 → logfile과 stdout에 write()
```

`Logger::VLogF()`는 verbosity를 적용하고 최대 4096-byte 문자열을 생성합니다. 기본 queue 용량은 250개 행입니다. Queue가 가득 차면 Worker는 빈 공간이 생길 때까지 대기합니다. 정상 종료 과정에서는 queue에 남은 메시지까지 출력합니다.

`-l <파일>` 옵션은 같은 메시지를 logfile과 stdout에 기록합니다. Logfile 동기 쓰기는 storage I/O 부하를 추가합니다.

> **소스 위치:** `src/sat.cc`의 `logprintf()`, `InitializeLogfile()`과 `src/logger.cc`의 `VLogF()`, `ThreadMain()`, `WriteAndDeleteLogLine()`

## `read`와 `reread` 해석

| 비교 결과 | 코드가 기록한 사실 | 분석 범위 |
|---|---|---|
| `read != expected`, `reread == expected` | 첫 상세 검사값에서 mismatch가 발생하고 두 번째 load에서 expected가 관찰됨 | 두 load 사이의 CPU, cache, coherency와 data path 상태 확인 |
| `read == reread`, 두 값 모두 `expected`와 다름 | 같은 mismatch 값이 두 번 관찰됨 | 마지막 write 이후 지속된 데이터 상태와 반복 재현성 확인 |
| `read != reread`, 두 값 모두 `expected`와 다름 | 서로 다른 mismatch 값이 연속 관찰됨 | CPU migration, cache 상태 변화와 transient data path 확인 |
| Checksum mismatch 후 상세 비교 일치 | Checksum 계산과 상세 비교 시점의 관찰값이 달라짐 | 두 검사 사이의 시간, CPU와 memory 상태 확인 |

Stressapptest는 상세 mismatch를 `Hardware Error`와 hardware incident로 집계합니다. `read error` 문자열은 `reread == expected` 조건에서 생성되는 software 분류입니다.

AArch64 공개 경로의 reread는 현재 cache 상태를 유지한 CPU load입니다. 위 표는 관찰값의 시간적 관계를 나타냅니다. Hardware 발생 위치는 CPU·cache PMU, interconnect, memory controller, PHY, DRAM과 RAS 정보를 함께 사용하여 판정합니다.

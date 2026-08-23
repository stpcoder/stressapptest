# 오류 검사와 로그 처리 과정

이 장은 메모리 Worker가 데이터 오류를 검출하고 로그를 출력하는 과정을 설명합니다. 설명 기준은 이 저장소의 Android AArch64 코드입니다.

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

`read`, `reread`, `expected`가 포함된 상세 로그는 64-bit 값의 mismatch를 검출할 때 생성됩니다. `--error-log-limit N`을 사용한 실행은 전체 Worker의 상세 보고를 N개로 제한합니다.

## Worker 검사 기능

| Worker | 주요 함수 | 수행 기능 |
|---|---|---|
| Fill Worker | `FillPage()` | Empty block에 expected pattern을 기록합니다. |
| Copy Worker | `CrcCopyPage()` | Source를 destination으로 복사하면서 source checksum을 계산합니다. |
| Warm Copy Worker | `CrcWarmCopyPage()` | CPU 연산을 포함한 copy와 checksum 검사를 수행합니다. |
| Invert Worker | `InvertThread::Work()`, `InvertPageUp()`, `InvertPageDown()` | `InvertThread::Work()`가 반전 전·후 checksum을 검사하고, 두 Invert 함수가 주소 방향에 따라 데이터를 반전하여 저장합니다. |
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
| `vaddr` | Mismatch가 발생한 가상 주소 |
| `patternname` | Expected 값을 만든 Pattern 이름 |
| `lastcpu` | Block의 마지막 writer CPU |
| `worker_name` | Mismatch를 검출한 Worker 종류 |
| `phase` | Worker 내부의 검사 단계 |
| `pattern_byte_offset` | SAT 작업 단위 시작점에 적용한 pattern 위치 이동값 |

한 번의 `CheckRegion()`은 최대 128개 오류 항목을 저장한 후 `ProcessError()`에 순서대로 전달합니다.

```cpp
const int kErrorLimit = 128;
struct ErrorRecord recorded[kErrorLimit];
```

> **소스 위치:** `src/worker.cc`의 `WorkerThread::CheckRegion()`

128개를 초과한 mismatch는 첫 128개 다음 위치부터 다시 검사합니다. 각 mismatch는 오류 수에 한 번 반영됩니다. 기본 memory 로그는 Worker별 첫 30개 항목을 상세 보고 대상으로 사용하며 `--error-log-limit`은 이 대상에 전역 예산을 적용합니다.

## 3단계: reread와 오류 메시지 생성

`ProcessError()`는 각 `ErrorRecord`를 다음 순서로 처리합니다.

```text
OsLayer::Flush(vaddr) 호출
 → 같은 가상 주소 load
 → 물리 주소 변환 시도
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

오류 메시지의 가상 주소와 물리 주소는 상세 검사에서 읽은 64-bit word의 시작 주소를 가리킵니다. `block_offset`은 그 word 안에서 처음 다른 byte의 위치까지 반영합니다. 따라서 주소 쌍은 같은 word를 나타내고, 정확한 mismatch byte 위치는 `sat_offset + block_offset`으로 계산합니다.

### AArch64 `Flush()` 실행 조건

`OsLayer::Flush()`는 `has_clflush_ == true` 조건에서 `FastFlush()`를 실행합니다.

```cpp
void OsLayer::Flush(void *vaddr) {
  if (has_clflush_) {
    OsLayer::FastFlush(vaddr);
  }
}
```

공개 AArch64 경로의 `has_clflush_` 값은 `false`입니다. 이 조건에서 `Flush()`는 cache 관리 명령 없이 반환하고, `reread`는 같은 가상 주소에 대한 두 번째 CPU load로 수행됩니다.

Invert Worker는 `FastFlushHint()` 경로에서 `DC CVAU`를 실행합니다. `DC CVAU`는 Data Cache line을 Point of Unification까지 clean하고 line의 valid 상태를 유지할 수 있습니다.

로그에는 두 CPU load가 각각 관찰한 값이 기록됩니다. 응답한 메모리 계층은 PMU, system cache monitor 또는 memory-controller counter로 확인합니다.

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

`Logger::VLogF()`는 최대 4096 byte 메시지를 생성합니다. 기본 queue는 250개 행이며, queue가 가득 차면 Worker는 빈 공간이 생길 때까지 대기합니다. 정상 종료 과정에서는 queue에 남은 메시지까지 출력합니다.

`-l <파일>` 옵션은 같은 메시지를 logfile과 stdout에 기록하며 기존 파일 끝에 새 로그를 추가합니다. Logfile은 동기 쓰기 플래그를 사용하므로 저장 장치 입출력이 시험 조건에 추가됩니다. 비교 실행에서는 로그 파일 사용 여부를 동일하게 유지합니다.

`--error-log-limit N`은 상세 `logprintf()` 출력만 제한합니다. ErrorDiagnoser 전달, 운영체제 오류 보고, 오류 수, reread, expected 복구와 `--stop_on_errors` 요청은 계속 처리합니다. 종료 시 다음 요약을 출력합니다.

```text
DIAG_ERROR_LOG limit=<N> detailed=<reported_count> suppressed=<omitted_count>
```

여러 Worker는 하나의 전역 예산을 공유합니다. 상세 보고에 포함되는 항목의 순서는 thread scheduling에 따라 달라질 수 있습니다.

> **소스 위치:** `src/sat.cc`의 `logprintf()`, `InitializeLogfile()`과 `src/logger.cc`의 `VLogF()`, `ThreadMain()`, `WriteAndDeleteLogLine()`

## `read`와 `reread` 해석

| 비교 결과 | 코드가 기록한 사실 | 분석 범위 |
|---|---|---|
| `read != expected`, `reread == expected` | 첫 상세 검사값에서 mismatch가 발생하고 두 번째 load에서 expected가 관찰됨 | 두 load 사이의 CPU, cache, coherency와 data path 상태 확인 |
| `read == reread`, 두 값 모두 `expected`와 다름 | 같은 mismatch 값이 두 번 관찰됨 | 마지막 write 이후 지속된 데이터 상태와 반복 재현성 확인 |
| `read != reread`, 두 값 모두 `expected`와 다름 | 서로 다른 mismatch 값이 연속 관찰됨 | CPU migration, cache 상태 변화와 transient data path 확인 |
| Checksum mismatch 후 상세 비교 일치 | Checksum 계산과 상세 비교 시점의 관찰값이 달라짐 | 두 검사 사이의 시간, CPU와 memory 상태 확인 |

Stressapptest는 상세 mismatch를 `Hardware Error`로 출력하고 오류 건수에 합산합니다. `read error`는 `reread == expected` 조건에서 붙는 로그 분류입니다.

AArch64 공개 경로의 reread는 현재 cache 상태에서 수행하는 CPU load입니다. 로그만으로 원인 위치를 확정할 수 없습니다. CPU·cache PMU, interconnect, memory controller, PHY, DRAM과 RAS 정보를 함께 확인합니다.

## Worker와 검사 단계 확인

Memory mismatch 로그에는 오류를 검출한 Worker와 검사 단계가 포함됩니다.

초기 실행 단계는 다음 `DIAG` 로그로 구분합니다.

```text
DIAG phase=prefault_begin
DIAG phase=prefault_end
DIAG phase=preset_fill_begin
DIAG phase=preset_fill_end
DIAG phase=initial_fill_begin
DIAG phase=initial_fill_end
DIAG phase=post_fill_delay_begin
DIAG phase=post_fill_delay_end
DIAG phase=post_fill_check_begin
DIAG phase=post_fill_check_end
DIAG phase=queue_split_begin
DIAG phase=queue_split_end
DIAG phase=runtime_start_delay_begin
DIAG phase=runtime_start_delay_end
DIAG phase=runtime_skipped reason=stop_on_errors
DIAG phase=final_check_skipped reason=<option|stop_on_errors>
```

선택 단계의 로그는 해당 옵션을 지정한 실행에서 출력됩니다. `prefault`, `preset_fill`, `post_fill_delay`, `post_fill_check`와 `runtime_start_delay`는 각각 `--prefault-pages`, `--fill-preset`, `--post-fill-delay`, `--verify-after-fill`과 `--runtime-start-delay`에 대응합니다. `runtime_skipped`는 초기 검사 중 `--stop_on_errors` 종료 요청이 기록된 경우에 출력됩니다. `final_check_skipped`의 `reason`은 명시적 생략 시 `option`, mismatch 처리로 종료 요청이 기록된 경우 `stop_on_errors`입니다.

```text
worker:fill, phase:immediate_check
worker:post_fill, phase:full_check
worker:check, phase:runtime_check
worker:check, phase:final_check
worker:copy, phase:source_check
worker:copy, phase:destination_check
worker:invert, phase:precheck
worker:invert, phase:postcheck
```

각 단계의 의미는 다음과 같습니다.

| 로그 필드 | 검출 시점 |
|---|---|
| `fill/immediate_check` | `--fill-verify-every`가 개별 SAT 작업 단위의 Fill 직후 검사 |
| `post_fill/full_check` | 전체 Fill과 선택한 `--post-fill-delay`가 끝난 후 `--verify-after-fill`이 수행한 검사 |
| `check/runtime_check` | `-c`로 생성한 Check Worker 실행 중 |
| `check/final_check` | 실행 시간이 끝난 뒤 최종 검사 중 |
| `copy/source_check` | Source를 읽고 destination에 복사하면서 계산한 checksum 검사 |
| `copy/destination_check` | `--copy-verify-destination`이 Copy 직후 destination을 검사 |
| `invert/precheck` | 첫 번째 반전 저장을 시작하기 전 |
| `invert/postcheck` | 네 번째 반전 저장이 끝난 후 |

로그 필드 형식은 다음과 같습니다. 꺾쇠괄호 항목은 실행 중 수집한 값으로 교체됩니다.

```text
Hardware Error: miscompare on CPU <current_cpu>(<-<last_writer_cpu>) at <virtual_address>(<physical_address>:DIMM Unknown): read:<read_value>, reread:<reread_value>, expected:<expected_value>. '<pattern>'; <classification>, worker:<worker>, phase:<phase>, pattern_offset:<byte_offset>, sat_block:<index>,sat_offset:<offset>,block_offset:<offset>, ch:<value>,rk:<value>,sc:<value>,bg:<value>,bank:<value>,row:<value>,col:<value>, cur_mode:<mode>, cur_freq:<frequency>, ddr_freq(write=<value> read=<value> reread=<value>), block_history(last_writer:<writer>,generation:<N>,writer_thread:<N>,writer_cpu:<N>,write_age_us:<time>,freq_epoch_begin:<N>,freq_epoch_end:<N>,freq_span:<single|mixed>).
```

`worker`와 `phase`는 mismatch 검출 위치를 기록합니다. `sat_block`과 `sat_offset`은 SAT 작업 단위 위치를 기록하고, `block_offset`은 처음 다른 byte의 위치를 기록합니다. 가상 주소와 물리 주소는 해당 byte를 포함한 64-bit word의 시작 주소입니다. `ProcessError()`는 `read`와 `reread`를 출력한 후 해당 64-bit 위치를 expected로 복구합니다.

`block_history(...)`는 `--diag-block-history`를 지정한 실행에서 기본 메모리 mismatch 상세 행에 출력됩니다. `last_writer`는 해당 작업 단위에서 마지막으로 완료된 추적 write 단계입니다. Fill과 Copy는 작업 단위 전체의 완료를 기록하고, Invert는 `legacy` 또는 `full`로 선택한 범위의 네 pass 완료를 기록합니다. `writer_cpu`는 작업 완료 직후 `sched_getcpu()`로 확인한 CPU 표본입니다. `write_age_us`는 추적 write 완료부터 mismatch 보고까지의 단조 시간 차이입니다. 이 옵션은 작업 단위마다 별도 metadata를 할당하고 추적 write 완료 시 clock과 epoch를 기록합니다. Word 단위 expected 복구와 4 KiB 부분 복구는 이 이력을 갱신하지 않습니다. Tag 전용 로그와 File 전용 상세 로그에는 이 suffix가 없습니다. `--error-log-limit 0`을 함께 지정하면 metadata 기록 비용은 발생하고 상세 행은 출력되지 않습니다.

### DRAM 주파수 필드

| 필드 | 기록 시점 |
|---|---|
| `ddr_freq(write=...)` | 해당 SAT 작업 단위의 가장 최근 write 작업을 시작할 때 저장한 전달값 |
| `ddr_freq(read=...)` | `CheckRegion()`이 해당 64-bit word를 상세 검사하기 직전에 확인한 전달값 |
| `ddr_freq(reread=...)` | `ProcessError()`가 같은 주소를 다시 읽기 직전에 확인한 전달값 |
| `cur_mode` | 주파수 옵션 미사용 시 `none`, 한 값 사용 시 `fixed`, 여러 값 사용 시 `sweep` |
| `cur_freq` | 현재 구현에서 `ddr_freq(reread=...)`와 일치하는 시점의 전달값 |

Copy destination의 `write` 값은 복사를 시작할 때 저장합니다. Invert의 `postcheck`에 표시되는 `write` 값은 네 번째 반전 작업을 시작할 때 저장한 값입니다. `--diag-block-history`는 작업 단위 write 시작과 종료의 요청 epoch를 기록합니다. `freq_span:mixed`는 해당 구간에서 성공한 새 요청이 발생했음을 표시합니다. 각 pass의 실제 적용 주파수는 별도로 저장하지 않습니다.

`--ddr-freq`를 사용하면 제어 경로에 값을 전달할 때 다음 로그를 출력합니다.

```text
Log: DDR_FREQ write=<value> epoch=<request_generation> monotonic_us=<time> node=<control_path>
```

이 로그는 제어 경로의 `open`, `write`, `close`가 성공한 결과입니다. Epoch는 성공한 요청의 세대 번호입니다. 적용 완료 신호와 hardware acknowledgement는 포함하지 않습니다. DDR 옵션이 없는 실행의 block history epoch는 `0`이며 `freq_span:single`은 고정 주파수 판정을 의미하지 않습니다. 개별 메모리 load와 store는 매번 로그로 출력하지 않습니다. `--ddr-step 3`을 사용하는 sweep에서는 약 3초 간격으로 이 행이 추가됩니다. 실제 동작 주파수는 대상 시스템의 계측값으로 확인합니다.

주파수 요청값과 요청 epoch는 서로 다른 atomic 값에서 읽습니다. 전환 경계에서 두 값을 읽는 사이에 새 요청이 완료되면 한 로그에 인접한 두 세대의 값이 함께 기록될 수 있습니다. 주파수별 1차 비교는 `--ddr-freq <지원값>`을 사용한 고정 실행으로 수행하고, sweep epoch는 전환 구간을 확인하는 보조 정보로 사용합니다.

## 단계별 요약과 VM 상태

`--diag-phase-summary`는 먼저 해석된 실행 구성을 출력합니다.

이 절의 `DIAG_CONFIG`, `DIAG_CONFIG_OPTIONS`, `DIAG_SUMMARY`, `DIAG_VM`과 `DIAG_ERROR_LOG` 요약은 log level 5를 사용합니다. 기본 `-v 8`에서 출력되며, verbosity를 직접 지정할 때는 `-v 5` 이상을 사용합니다.

```text
DIAG_CONFIG blocks=<count> block_bytes=<bytes> queue=<fine|coarse> workers(fill=<N>,copy=<N>,invert=<N>,check=<N>) invert_range=<legacy|full> final_mode=<legacy_drain|separate|skip> final_threads=<N> ddr_mode=<none|fixed|sweep> ddr_count=<N> ddr_step_s=<seconds> ddr_sweep_phase=runtime
DIAG_CONFIG_OPTIONS pattern_offset=<bytes> fill_preset=<none|zero|one> prefault=<0|1> fill_direction=<up|down> fill_verify_every=<N> fill_yield_bytes=<bytes> verify_after_fill=<0|1> post_fill_delay_s=<seconds> runtime_start_delay_s=<seconds> copy_verify_destination=<0|1> strict=<0|1> warm=<0|1> tag_mode=<0|1> cpu_stress=<N> affinity=<0|1> block_history=<0|1> vm_stats=<0|1> error_log_limit=<N|-1> stop_on_errors=<0|1> dram_map=<none|lpddr-v1>
```

`final_mode:legacy_drain`은 기존 Runtime Check의 종료 drain을 유지합니다. `separate`는 명시한 `--final-check-threads`가 종료 검사를 담당하며, `skip`은 종료 검사를 생략합니다. DDR 목록의 순환은 Runtime 단계에서 시작합니다. `DIAG_CONFIG_OPTIONS`는 데이터 접근 수, 순서, 대기 또는 오류 이후 timing에 영향을 주는 설정을 해석된 값으로 기록합니다. `error_log_limit:-1`은 상세 로그 제한을 사용하지 않는 설정입니다.

종료 시에는 Worker가 처리한 논리 작업량을 단계별로 출력합니다.

```text
DIAG_SUMMARY phase=<phase> blocks=<count> read_bytes=<bytes> write_bytes=<bytes> checksum_mismatch_regions=<count> word_mismatches=<count> first_error_us=<time> first_error_epoch=<request_generation> first_error_worker_bytes=<bytes>
```

표시 byte에는 cache traffic, 상세 reread, expected 복구와 로그 I/O가 포함되지 않습니다. `word_mismatches`의 집계 범위는 `CheckRegion()`이 확인한 일반 메모리 word 차이입니다. Tag mismatch는 Tag 상세 로그와 전체 error count로 집계됩니다. `first_error_*`는 해당 phase에서 처음 집계된 오류를 기록합니다. 일반 메모리 mismatch는 `CheckRegion()`이 첫 word 차이를 확인한 시점에 기록합니다. 상세 word 차이 시점을 받지 못한 오류 경로는 작업 단위 집계 시점에 대체 기록합니다. `first_error_us`는 `Sat::Initialize()` 시작부터 기록 시점까지의 단조 시간입니다. `first_error_epoch`는 그 시점에 읽은 software 요청 세대입니다. `first_error_worker_bytes`는 해당 Worker가 같은 phase에서 오류 작업 단위를 시작하기 전에 완료한 논리 read·write 합계입니다. Epoch는 hardware 적용 완료를 의미하지 않습니다.

`--diag-vm-stats`는 allocation, Fill, queue 구성, Runtime과 final check 경계에서 다음 정보를 출력합니다.

```text
DIAG_VM phase=<phase> backend=<allocator> mapping=<static|dynamic> os_page_bytes=<bytes> minor_faults=<count> minor_delta=<count> major_faults=<count> major_delta=<count> vm_rss_kb=<value> anon_huge_kb=<value>
```

이 옵션은 단계 경계에서 운영체제 통계 파일을 읽습니다. 접근 권한 또는 kernel 지원이 없는 값은 `-1`로 출력됩니다.

종료 검사를 수행한 실행의 마지막 VM 행은 `phase=final_check`를 사용합니다. `--skip-final-check` 또는 `--stop_on_errors`로 종료 검사를 생략한 실행은 `phase=final_check_skipped`를 사용합니다.

현재 구현은 `--ddr-node` 경로에 `{class:ddr, res:fixed, val:<값>}` 형식의 한 줄을 기록합니다. 대상 kernel interface가 이 형식을 지원하지 않으면 open 또는 write 오류가 출력되고 실행 준비가 실패합니다. `<값>`의 단위와 허용 범위는 대상 interface 규격을 따릅니다.

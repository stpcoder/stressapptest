# 단계별 오류 검출과 옵션 영향 분석

이 장은 메모리 준비부터 종료 검사까지의 실행 단계를 구분하고, 각 단계에서 사용할 진단 옵션과 시험 조건 변화를 정리합니다. OneZero256은 실행 순서를 설명하기 위한 예시 Pattern으로 사용합니다.

## 옵션 영향과 구현 위험 판단 기준

메모리 진단 옵션은 접근 순서, read·write 수, 실행 시간 또는 Worker 동시성을 변경할 수 있습니다. 이 장에서는 다음 두 항목을 구분합니다.

| 구분 | 의미 |
|---|---|
| 시험 조건 변화 | 옵션의 목적에 포함된 read·write 증가, 순서 변경, 대기 또는 Worker 수 변경 |
| 구현 위험 | 범위 초과 접근, queue 상태 손상, expected 계산 불일치, 중복 처리와 누락 처리로 시험 결과를 왜곡하는 동작 |

추가 read·write 또는 대기를 만드는 진단 기능은 기본적으로 비활성입니다. Fill 주소 방향은 `up`, Fill Worker와 종료 검사 Worker는 각각 8개이며 DDR 변경 간격은 3초입니다. Invert 처리 범위의 기본값은 기존 공개 코드와 동일한 `legacy`입니다. 기본 Fill은 낮은 주소에서 높은 주소로 연속 기록하고 Fill 직후 즉시 검사와 중간 yield를 수행하지 않습니다.

### 옵션 영향 분류

| 분류 | 옵션 | 적용 기준 |
|---|---|---|
| 기존 동작 유지 | 옵션 미지정, `--invert-range legacy` | 기본 Fill, Invert 주소 범위, queue 상태와 Runtime Check 종료 drain 유지 |
| 관측 처리 추가 | `--diag-phase-summary`, `--diag-vm-stats` | 데이터 접근 규칙을 유지하며 local counter, 단계 경계 system 정보와 로그 추가 |
| 시험 조건 변경 | `--prefault-pages`, Fill 제어, 검사 추가, 대기, `--invert-range full`, 종료 검사 제어, DDR 제어 | read·write 수, 주소 순서, Worker 동시성, cache·TLB 상태, 실행 시간 또는 hardware 상태 변경 |
| 오류 이후 조건 변경 | `--error-log-limit` | 오류 판정·reread·복구를 유지하며 Logger I/O와 오류 이후 timing 변경 |

`--diag-block-history`는 관측 정보를 생성하며 작업 단위별 metadata와 write 완료 시점의 clock 확인을 추가합니다. 작은 `-p`에서는 metadata가 커지므로 시험 조건 변경 항목으로 관리합니다.

## 전체 실행 단계와 검출 위치

```text
명령행 옵션 확인
  → 메모리 영역 확보
  → 선택: 운영체제 페이지 사전 접근
  → 선택: 사전 데이터 전체 기록
  → 최종 Pattern 전체 기록
  → 선택: Fill 중 즉시 검사
  → 선택: Fill 이후 대기
  → 선택: Fill 전체 결과 검사
  → Valid·Empty queue 구성
  → 선택: Runtime 시작 대기
  → Runtime Check·Copy·Invert
  → mismatch 상세 비교·reread·복구·로그
  → 선택: 종료 시점 Valid 검사
```

### 단계 1: 명령행 옵션과 실행 환경 확인

`Sat::ParseArgs()`가 숫자 범위, 문자열과 정렬 조건을 확인합니다. 이 단계의 실패는 `Process Error`로 출력되며 Pattern 데이터 비교는 시작되지 않습니다.

확인 항목:

- `-M`, `-p`, Worker 수와 실행 시간
- Pattern 이름 또는 ID
- Pattern offset의 4 byte 정렬
- Fill yield 크기의 64 byte 정렬과 SAT 작업 단위 범위
- DRAM 주파수 목록과 변경 간격의 입력 형식

DRAM 제어 경로의 write 권한은 초기 주파수를 전달하는 `ApplyDramFrequency()` 실행 시점에 확인합니다.

### 단계 2: 메모리 영역 확보

`AllocateTestMem()`이 시험용 가상 주소 범위를 확보합니다. 일반 Android ARM64 경로는 anonymous `mmap()`을 사용할 수 있습니다. 이 시점에는 전체 물리 페이지가 할당되지 않을 수 있습니다.

발생 가능한 상태:

- 주소 공간 또는 메모리 부족에 따른 할당 실패
- 시험 크기와 운영체제 여유 메모리의 충돌
- LMKD 또는 다른 프로세스와의 메모리 경쟁

이 단계에는 expected Pattern 비교가 없습니다.

### 단계 3: 운영체제 페이지 사전 접근

`--prefault-pages`를 지정하면 메인 스레드가 각 SAT 작업 단위의 offset 0부터 운영체제 page 크기 간격으로 `0x00`을 기록합니다. `-p`가 운영체제 page보다 작으면 각 SAT 작업 단위의 첫 byte를 기록합니다. 아직 물리 페이지가 연결되지 않은 주소에서는 이 write가 page fault와 물리 페이지 할당을 유도합니다.

로그:

```text
DIAG phase=prefault_begin
DIAG phase=prefault_end
```

이 단계의 write는 최종 Pattern Fill에서 덮어씁니다. 옵션을 사용하면 미할당 물리 페이지의 할당 시점, 페이지 배치, TLB·cache 상태, 초기 write 이력과 초기화 시간이 변경됩니다.

<sub><em>First-touch: 예약된 가상 페이지에 처음 접근하는 동작입니다. 아직 연결된 물리 페이지가 없으면 kernel이 이 시점에 물리 페이지를 할당합니다.</em></sub>

### 단계 4: 사전 데이터 전체 기록

`--fill-preset zero`와 `--fill-preset one`은 전체 시험 영역에 각각 `0x00`과 `0xFF`를 기록합니다. 사전 채움 단계가 모든 SAT 작업 단위를 완료한 후 최종 Pattern 기록 단계가 시작됩니다.

로그:

```text
DIAG phase=preset_fill_begin
DIAG phase=preset_fill_end
```

사전 채움은 전체 시험 영역에 CPU store를 한 번 추가합니다. 중간 데이터의 checksum은 검사하지 않습니다. 이 단계는 선행 데이터 이력과 실행 시간을 변경합니다. DRAM 도달량은 memory-controller 계측값으로 확인합니다.

### 단계 5: 최종 Pattern 전체 기록

Fill Worker가 Empty SAT 작업 단위를 가져와 OneZero256을 기록합니다. 기본 SAT 작업 단위는 1 MiB이며, 한 작업 단위 안에서는 64-bit store가 연속 실행됩니다.

로그:

```text
DIAG phase=initial_fill_begin
DIAG phase=initial_fill_end
```

Fill은 Pattern을 기록하며 checksum을 검사하지 않습니다. 데이터 비교는 이후 검사 단계에서 시작됩니다.

관련 옵션:

| 옵션 | 변경되는 조건 |
|---|---|
| `--fill-threads N` | 동시에 Pattern을 기록하는 Fill Worker 수 |
| `--fill-direction up\|down` | SAT 작업 단위 내부의 저장 주소 진행 방향 |
| `--fill-yield-bytes N` | N byte 기록마다 `sched_yield()`를 호출하는 연속 write 길이 |
| `--pattern-byte-offset N` | Pattern 전환 위치와 expected checksum 위치 |

### 단계 6: Fill 중 즉시 검사

`--fill-verify-every N`은 1부터 시작하는 SAT 작업 단위 순번을 기준으로 N의 배수에 해당하는 작업 단위를 queue에 반환하기 전에 검사합니다. `N=1`은 모든 SAT 작업 단위를 검사합니다. 같은 N 값은 반복 실행에서 같은 논리 주소 집합을 선택합니다.

로그:

```text
worker:fill, phase:immediate_check
```

선택한 작업 단위마다 1 MiB read와 checksum 계산이 추가됩니다. Mismatch 처리에는 상세 비교, reread, expected 복구와 로그 출력이 포함됩니다. 로그 단계는 `fill/immediate_check`입니다.

### 단계 7: Fill 이후 대기와 전체 검사

`--post-fill-delay N`은 최종 Fill 종료부터 post-fill 검사 또는 queue 구성까지 대기합니다. `--verify-after-fill`은 대기 이후 모든 SAT 작업 단위를 각각 한 번 검사합니다.

로그:

```text
DIAG phase=post_fill_delay_begin
DIAG phase=post_fill_delay_end
DIAG phase=post_fill_check_begin
DIAG phase=post_fill_check_end
worker:post_fill, phase:full_check
```

`--verify-after-fill`은 전체 시험 영역 read, checksum 계산과 mismatch 복구를 추가합니다. FineLock queue는 논리 offset 순서로 한 항목씩 조회합니다. OneLock queue는 임의 순서로 항목을 가져오고 검사한 항목을 기존 Empty queue에 임시 보관한 뒤 Valid queue로 복원합니다. 두 경로 모두 SAT 작업 단위 하나의 metadata만 보유합니다. `--stop_on_errors`가 mismatch를 검출하면 현재 SAT 작업 단위의 상세 검사를 마친 뒤 post-fill 검사를 종료합니다. Queue 구성 후 Runtime Worker 생성을 생략합니다.

### 단계 8: Valid·Empty queue 구성

전체 SAT 작업 단위의 물리 주소와 region tag를 확인하고 Runtime용 Valid·Empty 상태를 구성합니다. FineLock queue는 약 2/5를 Empty로 설정하고 나머지를 Valid로 유지합니다.

로그:

```text
DIAG phase=queue_split_begin
DIAG phase=queue_split_end
```

Queue 구성은 SAT 작업 단위의 Pattern 상태와 lock 상태를 변경합니다. Valid 데이터 자체를 다시 기록하지 않습니다. Empty로 지정된 작업 단위의 기존 데이터는 이후 Copy destination write에서 교체됩니다.

### 단계 9: Runtime Worker 시작 전 대기

`--runtime-start-delay N`은 queue 구성이 끝난 후 Runtime Worker 생성 전까지 대기합니다.

로그:

```text
DIAG phase=runtime_start_delay_begin
DIAG phase=runtime_start_delay_end
```

이 옵션은 시험 메모리에 접근하지 않습니다. Runtime 시작까지의 경과 시간과 백그라운드 프로세스의 실행 기회가 증가합니다.

### 단계 10: Runtime Check

`-c N`은 N개의 Check Worker를 생성합니다. 각 Worker는 Valid SAT 작업 단위를 읽고 checksum을 검사한 후 다시 Valid queue에 반환합니다.

로그:

```text
worker:check, phase:runtime_check
```

Worker 수가 증가하면 동시 read 요청과 cache refill이 증가할 수 있습니다. 실제 memory-controller read 요청량은 cache 상태와 DMC 계측값으로 확인합니다.

기본 실행의 Runtime Check Worker는 종료 요청 이후 Valid queue를 끝까지 검사하고 완료 항목을 Empty로 이동합니다. `--final-check-threads N`을 명시하면 처리 중인 항목을 Valid로 반환하고 N개의 별도 종료 Check Worker가 검사를 담당합니다. `--skip-final-check`는 종료 drain을 수행하지 않습니다.

### 단계 11: Runtime Copy

`-m N`은 N개의 Copy Worker를 생성합니다. Copy Worker는 source Valid 작업 단위를 읽으면서 checksum을 계산하고 destination Empty 작업 단위에 기록합니다.

로그:

```text
worker:copy, phase:source_check
worker:copy, phase:destination_check
```

`source_check`는 Copy 중 source read에서 계산한 checksum 검사입니다. `--copy-verify-destination`을 지정하면 destination write 직후 전체 destination을 읽어 `destination_check`를 수행합니다.

### 단계 12: Runtime Invert

`-i N`은 N개의 Invert Worker를 생성합니다. 기본 strict mode에서 각 Worker는 시작 checksum 검사, 선택 범위의 네 번 반전 저장과 마지막 checksum 검사를 수행합니다. `--invert-range legacy`는 기존 공개 코드의 pointer 계산 범위를 사용하며 1 MiB 작업 단위에서 pass당 512 KiB를 처리합니다. `--invert-range full`은 `-p` 전체를 처리합니다. `-F`를 지정하면 반전 전·후 checksum 검사는 생략되고 네 번의 반전 저장은 유지됩니다.

로그:

```text
worker:invert, phase:precheck
worker:invert, phase:postcheck
```

`precheck`는 첫 반전 저장 전 상태를 검사합니다. `postcheck`는 네 번째 반전 저장 완료 후 상태를 검사합니다.

`--diag-phase-summary`를 사용하면 반전 단계에 다음 항목이 추가됩니다.

```text
DIAG_SUMMARY phase=invert/precheck ...
DIAG_SUMMARY phase=invert/rmw ...
DIAG_SUMMARY phase=invert/postcheck ...
```

`invert/rmw`의 read·write byte는 선택 범위에서 네 번씩 처리한 논리 byte입니다.

### 단계 13: Mismatch 상세 처리

Checksum mismatch가 발생하면 `CheckRegion()`이 64-bit 단위로 expected와 비교합니다. `ProcessError()`는 같은 주소를 reread하고 로그를 출력한 후 expected 값을 기록합니다.

추가 로그 필드:

| 필드 | 의미 |
|---|---|
| `sat_block` | 전체 시험 영역에서 SAT 작업 단위 번호 |
| `sat_offset` | 시험 영역 시작점에서 SAT 작업 단위 시작점까지의 byte offset |
| `block_offset` | SAT 작업 단위 시작점에서 mismatch byte까지의 offset |
| `worker`, `phase` | Mismatch를 검출한 Worker와 검사 단계 |

`ProcessError()`는 mismatch 위치에 expected를 기록합니다. 이 복구 이후의 검사는 복구 전 mismatch를 다시 집계하지 않을 수 있습니다.

`--diag-block-history`를 사용하면 마지막 추적 write를 완료한 Worker 종류, thread 번호, 완료 직후 CPU 표본, 완료 후 경과 시간과 DDR 요청 epoch 범위가 기본 memory mismatch 상세 로그에 추가됩니다. Fill과 Copy는 작업 단위 전체의 완료를 기록하고, Invert는 선택 범위의 네 pass 완료를 기록합니다. Word 단위 expected 복구와 4 KiB 부분 복구는 이 이력을 덮어쓰지 않습니다. Tag 전용 로그와 File 전용 상세 로그에는 block history suffix가 없습니다.

### 단계 14: 종료 시점 Valid 검사

기본 실행에서는 Runtime Check Worker가 정지 요청 이후 Valid queue를 검사하여 Empty로 이동합니다. 남은 Valid 항목은 Fill Worker 수와 같은 수의 보조 Check Worker가 검사합니다. 기본 Fill Worker 수는 8입니다.

로그:

```text
worker:check, phase:final_check
```

`--final-check-threads N`을 명시하면 Runtime Check의 종료 drain을 중지하고 N개의 별도 종료 검사 Worker를 사용합니다. `--skip-final-check`는 Runtime drain과 별도 검사를 생략하고 `DIAG phase=final_check_skipped reason=option`을 출력합니다. 두 옵션을 함께 지정하면 생략 설정이 우선합니다. Mismatch 처리 과정에서 `--stop_on_errors` 종료 요청이 기록되면 종료 검사를 생략하고 `reason=stop_on_errors`를 출력합니다. VM 진단을 함께 사용한 생략 실행은 `DIAG_VM phase=final_check_skipped`를 출력합니다.

## 옵션별 시험 조건 변화와 구현 검토

| 옵션 | 기본 상태 | 시험 조건 변화 | 구현 검토 결과 |
|---|---|---|---|
| `-P ID\|이름[, ...]` | 가중치 선택 | 지정한 Pattern을 입력 순서대로 SAT 작업 단위에 순환 배정 | 전체 이름 또는 0 기반 ID를 초기화 전에 확인. 여러 Fill Worker는 원자적 cursor로 중복 없이 다음 Pattern을 선택. Worker 실행 순서에 따라 Pattern이 배정되는 논리 주소는 달라질 수 있음 |
| `--prefault-pages` | 비활성 | SAT 작업 단위마다 운영체제 page 크기 간격의 1 byte write, page fault·allocation 순서·cache·TLB 변경 | 각 write offset은 SAT 작업 단위 범위 안에 있으며 최종 Fill이 데이터를 덮어씀. `-p`가 운영체제 page보다 작으면 작업 단위마다 첫 byte를 기록. 메모리 부족은 Fill보다 먼저 관찰될 수 있음 |
| `--fill-preset zero\|one` | `none` | 시험 영역 전체 CPU store 단계 1회 추가, 데이터 이력·실행 시간·queue 선택 상태 변경 | 완료 작업 단위를 Valid로 표시하여 중복 선택을 방지하고 전체 완료 후 모두 Empty로 복원. 사전 채움은 `-P` Pattern 선택 순번을 진행시키지 않음 |
| `--fill-threads N` | 8 | 병렬 write 수와 queue 경쟁 변경. 종료 옵션 미지정 시 보조 종료 검사 Worker 수도 변경 | `--final-check-threads`를 명시하면 종료 검사 수와 분리. 생성 성공 Worker만 join. 허용 범위는 1~256이며 온라인 CPU 수 이하를 권장 |
| `--fill-direction down` | `up` | 저장 방향과 hardware prefetch·write 결합 순서 변경 | 주소별 Pattern index를 유지하여 최종 byte 배열과 expected checksum이 같음. 기본 최적화 Fill loop에서 주소별 Pattern 계산 loop로 전환 |
| `--fill-yield-bytes N` | 비활성 | N byte마다 `sched_yield()` 호출, 연속 write 길이와 대역폭 변경 | 64 byte 배수와 SAT 작업 단위 이하만 허용. 기본 최적화 Fill loop에서 주소별 Pattern 계산 loop로 전환. `sched_yield()`는 context switch를 보장하지 않음 |
| `--fill-verify-every N` | 비활성 | 선택 블록의 즉시 read·checksum·오류 복구 추가 | Worker가 entry를 독점 보유한 상태에서 검사. 논리적 작업 단위 번호가 N의 배수인 대상을 선택하며 N이 전체 개수보다 크면 선택 대상이 없음 |
| `--pattern-byte-offset N` | 0 | Pattern 전환 위치와 Fill 계산 경로 변경 | 4 byte 배수만 허용하고 Pattern 데이터와 사전 계산 checksum에 같은 offset 적용. 0 이외의 값은 주소별 Pattern index 계산이 추가되어 Fill 처리 시간이 달라질 수 있음 |
| `--post-fill-delay N` | 0 | Fill과 첫 검사 사이의 경과 시간 변경 | 대기 중 memory Worker가 실행되지 않음. 초기 Fill에서 종료 요청이 기록되면 대기 생략. Runtime signal handler 설치 전에 실행 |
| `--verify-after-fill` | 비활성 | 전체 영역 read와 checksum 추가, 오류 word 복구, Runtime 시작 cache 상태 변경 | SAT 작업 단위 하나의 metadata만 보유. FineLock은 논리 offset 조회 후 즉시 mapping 해제하고 queue RNG를 유지. OneLock은 기존 Empty queue를 임시 저장소로 사용하므로 queue 순서와 전역 난수 상태가 변경됨 |
| `--runtime-start-delay N` | 0 | Queue 구성과 Runtime 시작 사이의 경과 시간 변경 | 대기 구간에서 시험 메모리 접근을 추가하지 않음. 초기 검사에서 종료 요청이 기록되면 대기 생략. Runtime signal handler 설치 전에 실행 |
| `--copy-verify-destination` | 비활성 | Copy destination 전체 read와 checksum 추가 | Copy Worker가 destination entry를 독점 보유한 상태에서 검사 |
| `--invert-range full` | `legacy` | Invert pass 범위를 기존 계산값에서 SAT 작업 단위 전체로 확대 | 단위 테스트가 1회 up·down 변경 범위, 범위 밖 guard, 네 번 처리 후 원본 복구를 `-p 1024`, `2048`, `4096`, `1 MiB`에 대해 확인 |
| `--diag-phase-summary` | 비활성 | 시작 시 해석된 설정 2행 출력, 작업 단위 완료마다 Worker local 정수 counter 갱신 | Worker별 배열 사용. 전역 합산 lock은 Worker 종료 후 한 번 사용. 일반 memory mismatch는 첫 word 차이에서 기록하고 대체 경로는 작업 단위 집계 시점에 기록. 일반 load·store 순서는 유지하며 실행 시간이 소폭 증가할 수 있음 |
| `--diag-vm-stats` | 비활성 | 단계 경계에서 `getrusage()`와 `/proc` read 추가 | 시험 memory hot loop 접근 없음. 프로세스 전체 통계를 표시하며 `/proc` read와 Logger 활동은 다음 snapshot의 delta에 포함될 수 있음. 권한 제한 값은 `-1` 출력 |
| `--diag-block-history` | 비활성 | 작업 단위별 metadata 배열, 완료 write마다 epoch 조회와 monotonic clock 확인 추가 | `page_entry` 크기는 유지. 일반적인 64-bit build의 metadata는 작업 단위당 약 48 B이며 `-M 1024 -p 1024`에서 약 48 MiB 사용. 활성화한 실행은 동일 옵션끼리 처리량 비교 |
| `--error-log-limit N` | 제한 없음 | 오류 발생 후 상세 `logprintf()` 출력을 N개로 제한 | ErrorDiagnoser·외부 보고, 오류 수, reread, expected 복구와 `--stop_on_errors` 요청은 유지. 종료 시 상세 출력 수와 생략 수 기록 |
| `--final-check-threads N` | 명시하지 않음 | Runtime Check 종료 drain을 별도 N개 Worker 검사로 변경 | Runtime Worker 종료 후 생성. 생성 성공 Worker만 join. 허용 범위는 1~256이며 온라인 CPU 수 이하를 권장 |
| `--skip-final-check` | 비활성 | 종료 read와 검출 범위 제거 | Runtime Check 종료 drain과 별도 종료 Check Worker 생성을 생략하며 Runtime 종료 시점의 Valid 데이터는 검사 범위에서 제외 |
| `--stop_on_errors` | 비활성 | 상세 mismatch 처리 후 종료 요청 기록 | 초기 검사에서는 나머지 Fill과 queue 구성 후 Runtime 생략. Runtime Worker는 현재 반복을 정리한 뒤 새 작업을 시작하지 않음. 제어 반복문은 Worker를 정리하고 종료 검사 생략 |
| `--ddr-freq`, `--ddr-step` | 비활성 | 대상 주파수 상태와 전환 시점 변경 | 초기 단계는 첫 값 유지, Runtime에서 목록 순환. 제어 경로에 전달한 값을 로그에 저장. 실제 적용값은 대상 시스템 계측값으로 확인. Kernel write timeout은 없으며 종료 시 마지막 요청값 유지 |
| `--ddr-node PATH` | Build 기본 경로 | 주파수 요청을 기록할 kernel interface 변경 | `--ddr-freq`를 함께 지정한 실행에서만 `{class:ddr, res:fixed, val:<값>}` 형식의 한 줄을 기록. 경로가 비어 있는 입력은 거부하며 `<값>`의 단위·허용 범위와 write 권한은 대상 interface 규격에서 확인 |
| `--dram-map lpddr-v1` | `none` | 오류 로그의 DRAM 좌표 필드에 선택한 프로필의 변환 결과 기록 | Memory access를 추가하지 않음. 프로필을 사용하지 않거나 물리 주소를 확인할 수 없으면 좌표 필드에 `unknown` 기록. 대상 시스템 주소 매핑과의 일치 여부를 별도로 확인 |

### 기본 실행 경로 확인

진단 옵션을 지정하지 않은 Fill은 낮은 주소부터 높은 주소까지 중간 대기 없이 연속 기록합니다. 이 경로는 원본 Pattern index 계산식을 사용합니다. 종료 옵션을 지정하지 않으면 Runtime Check의 기존 종료 drain과 `--fill-threads` 수의 보조 종료 검사를 유지합니다. `--final-check-threads`를 명시한 실행만 종료 검사 수를 분리합니다. Invert는 `legacy` 범위를 사용합니다.

비활성 상태의 phase summary와 block history는 배열을 할당하지 않습니다. Block history용 epoch 조회와 시간 확인도 수행하지 않습니다. Hot loop에는 옵션 상태를 확인하는 boolean 분기가 남으므로 명령 실행 시간과 thread scheduling의 완전한 일치는 보장하지 않습니다. 데이터 배열, expected 계산, queue 상태와 오류 판정 규칙은 기존 경로를 유지합니다.

Invert의 `legacy` workload는 기존 주소 범위를 유지합니다. 현재 처리량 출력은 실제 선택 범위의 네 번 read와 네 번 write를 사용합니다. 기존 `GetCopiedData() × 4` 통계와 비교하면 1 MiB `legacy` 실행의 보고 byte가 절반으로 표시됩니다. 이 항목은 workload 변경이 포함되지 않은 처리량 계산 교정입니다.

### 옵션 조합 확인

| 옵션 조합 | 실행 결과 |
|---|---|
| `--fill-preset` + `--fill-direction` | 사전 채움과 최종 Pattern Fill에 같은 주소 방향 적용 |
| `--fill-preset` + `--fill-yield-bytes` | 사전 채움과 최종 Pattern Fill에 같은 yield 간격 적용 |
| `--fill-preset` + 여러 `-P` Pattern | 사전 채움은 Pattern 선택 순번을 진행시키지 않음. 최종 Fill은 입력 목록의 첫 항목부터 선택하며, 여러 Fill Worker의 작업 단위 획득 순서에 따라 논리 주소별 Pattern 배정이 달라질 수 있음 |
| `--fill-verify-every` + `--verify-after-fill` | 선택 작업 단위를 Fill 직후 검사한 뒤 전체 post-fill 검사를 추가 수행. 앞선 검사가 cache 상태와 오류 복구 상태를 변경 |
| `--skip-final-check` + `--final-check-threads` | 생략 설정이 우선하므로 Runtime drain과 별도 종료 Worker를 사용하지 않음 |
| `--copy-verify-destination` + `-m 0` | Copy Worker가 없어 destination 검사를 실행하지 않음 |
| `--copy-verify-destination` + `-F` | `memcpy()` destination write 이후 readback 검사를 실행 |
| `--invert-range legacy` + `-p 1024\|2048` | 반전 pass의 처리 byte가 0. Precheck와 postcheck는 strict mode에서 실행 |
| `--invert-range full` + `-i 0` | Invert Worker가 없어 범위 설정을 사용하지 않음 |
| `--diag-block-history` + `--ddr-freq` | 작업 단위 write 시작·종료 사이의 요청 epoch를 저장. 두 epoch가 다르면 `freq_span:mixed` 출력 |
| `--diag-block-history` + `--error-log-limit 0` | History metadata와 write 경로 비용은 발생하지만 상세 mismatch 행이 없어 history 내용은 출력되지 않음 |
| `--error-log-limit 0` + mismatch | 상세 memory mismatch 행을 생략하고 오류 집계·reread·복구를 계속 수행 |
| `--runtime-start-delay` + `-m 0 -i 0 -c 0` | 초기 Fill과 queue 구성 후 지정 시간 대기. Runtime Memory Worker 수는 0. 기본 종료 검사는 유지하며 `--skip-final-check`로 제외 |
| `--stop_on_errors` + 초기 검사 옵션 | 현재 상세 검사와 나머지 Fill의 queue 구성을 마친 후 Runtime Worker 생성을 생략. Fill 이후 대기·post-fill 검사·Runtime 전 대기는 종료 요청 시 생략. `DIAG phase=runtime_skipped` 출력 |

단계 분리 실행에서는 `--fill-verify-every`와 `--verify-after-fill`을 서로 다른 실행에 사용합니다. 각 검사 옵션은 사전 cache invalidate 없이 현재 cacheable mapping을 읽으며 mismatch 처리에서 expected 복구 write를 수행합니다.

DDR 주파수 값과 요청 epoch는 각 작업 시점에 별도로 읽습니다. 전환 경계에서 두 값을 읽는 사이에 새 요청이 완료되면 서로 다른 세대의 값이 한 작업 로그에 포함될 수 있습니다. 주파수별 1차 판정은 fixed 별도 실행을 사용하고, sweep epoch는 전환 구간 확인용으로 사용합니다.

## OneZero256 단계별 실행 예시

OneZero256은 단계 구분을 설명하기 위한 실행 예시입니다. 각 명령은 `/data/local/tmp`에 단계별 로그 파일을 저장합니다. 비교 실행에서는 시작 온도, CPU affinity, 백그라운드 부하와 전원 조건을 동일하게 유지합니다.

`-l`은 기존 파일의 끝에 로그를 추가합니다. 재실행 전 해당 단계의 원격 로그를 삭제하거나 새로운 파일명을 사용합니다.

```bash
adb shell 'rm -f /data/local/tmp/sat-stepNN.log'
```

각 실행이 끝난 후 `NN`을 실제 단계 번호로 바꾸어 git에서 제외된 로컬 디렉터리에 복사합니다.

```bash
mkdir -p .private/logs
adb pull /data/local/tmp/sat-stepNN.log .private/logs/
```

오류 로그에는 실행 주소, 데이터 비교값, 선택형 DRAM 좌표와 주파수 요청값이 포함될 수 있습니다. `.private/`와 저장소 최상위의 `sat-step*.log`는 `.gitignore`에 등록되어 있습니다. `-l`은 동기 파일 쓰기를 사용하므로 저장 장치 입출력이 시험 조건에 추가됩니다. 로그 파일 사용 여부를 비교 조건마다 동일하게 유지합니다.

3~10, 15, 16단계는 Runtime 메모리 Worker를 생성하지 않습니다. 일반 모드의 메인 제어 반복문은 최대 약 5초 간격으로 종료 시점을 확인하므로 `-s 1`은 전체 프로세스 실행 시간 1초를 의미하지 않습니다.

### 1. 기준 실행

```bash
adb shell '/data/local/tmp/stressapptest -M 1024 -s 600 -P OneZero256 -m 4 -i 4 -c 0 --invert-range legacy --printsec 10 -l /data/local/tmp/sat-step01.log'
```

초기 Fill, Copy Worker 4개, Invert Worker 4개와 종료 시점 Valid 검사를 포함합니다. `legacy`를 명시하여 기존 공개 코드의 Invert 주소 범위를 고정합니다. 각 후속 단계는 이 명령의 조건을 기준으로 검출 위치를 분리합니다.

### 2. 종료 검사 제외

```bash
adb shell '/data/local/tmp/stressapptest -M 1024 -s 600 -P OneZero256 -m 4 -i 4 -c 0 --printsec 10 --skip-final-check -l /data/local/tmp/sat-step02.log'
```

`check/final_check` 단계를 제외합니다. 로그에는 Fill과 Runtime에서 검출한 항목만 남습니다.

### 3. SAT 작업 단위별 Fill 직후 검사

```bash
adb shell '/data/local/tmp/stressapptest -M 1024 -s 1 -P OneZero256 -m 0 -i 0 -c 0 --printsec 1 --fill-verify-every 1 --skip-final-check -l /data/local/tmp/sat-step03.log'
```

각 1 MiB SAT 작업 단위를 기록한 직후 같은 Worker가 checksum을 검사합니다. 확인할 로그 단계는 `worker:fill, phase:immediate_check`입니다.

### 4. 전체 Fill 완료 후 검사

```bash
adb shell '/data/local/tmp/stressapptest -M 1024 -s 1 -P OneZero256 -m 0 -i 0 -c 0 --printsec 1 --verify-after-fill --skip-final-check -l /data/local/tmp/sat-step04.log'
```

전체 영역의 Pattern 기록이 끝난 후 단일 Check Worker가 모든 SAT 작업 단위를 검사합니다. 확인할 로그 단계는 `worker:post_fill, phase:full_check`입니다. 3단계와 4단계는 별도 실행하여 즉시 검사의 추가 read 영향을 분리합니다.

### 5. Fill 이후 대기 시간 적용

```bash
adb shell '/data/local/tmp/stressapptest -M 1024 -s 1 -P OneZero256 -m 0 -i 0 -c 0 --printsec 1 --post-fill-delay 10 --verify-after-fill --skip-final-check -l /data/local/tmp/sat-step05.log'
```

마지막 Pattern write와 post-fill 검사 사이에 10초를 둡니다. 비교 실행에서는 `--post-fill-delay` 값을 `0`, `1`, `10`, `30`으로 변경합니다.

### 6. Fill Worker 수 변경

```bash
adb shell '/data/local/tmp/stressapptest -M 1024 -s 1 -P OneZero256 -m 0 -i 0 -c 0 --printsec 1 --fill-threads 1 --verify-after-fill --skip-final-check -l /data/local/tmp/sat-step06.log'
```

초기 Pattern 기록을 Fill Worker 1개로 수행합니다. 비교 실행에서는 `--fill-threads` 값을 `1`, `2`, `4`, `8`로 변경합니다. 이 명령은 `--skip-final-check`를 사용하므로 종료 검사 Worker 수의 영향이 없습니다. 종료 검사를 포함한 실행에서 Fill 동시성만 비교할 때는 모든 조건에 같은 `--final-check-threads N`을 명시합니다.

### 7. 연속 Fill 기록 길이 변경

```bash
adb shell '/data/local/tmp/stressapptest -M 1024 -s 1 -P OneZero256 -m 0 -i 0 -c 0 --printsec 1 --fill-yield-bytes 4096 --verify-after-fill --skip-final-check -l /data/local/tmp/sat-step07.log'
```

각 Fill Worker는 4 KiB를 기록할 때마다 `sched_yield()`를 호출합니다. 비교 실행에서는 옵션을 생략한 연속 1 MiB 기록과 `4096`, `65536`, `262144` byte 간격을 사용합니다.

### 8. Fill 주소 진행 방향 변경

```bash
adb shell '/data/local/tmp/stressapptest -M 1024 -s 1 -P OneZero256 -m 0 -i 0 -c 0 --printsec 1 --fill-direction down --verify-after-fill --skip-final-check -l /data/local/tmp/sat-step08.log'
```

각 SAT 작업 단위를 높은 주소에서 낮은 주소 방향으로 기록합니다. 주소별 Pattern index를 유지하므로 완료 후 데이터 배열과 expected checksum은 `up` 조건과 같습니다.

### 9. 운영체제 페이지 사전 접근

```bash
adb shell '/data/local/tmp/stressapptest -M 1024 -s 1 -P OneZero256 -m 0 -i 0 -c 0 --printsec 1 --prefault-pages --verify-after-fill --skip-final-check -l /data/local/tmp/sat-step09.log'
```

메인 스레드가 각 SAT 작업 단위를 운영체제 page 크기 간격으로 기록한 후 Fill Worker를 시작합니다. Fill Worker가 수행하는 초기 접근 조건과 메인 스레드의 순차 사전 접근 조건을 비교합니다.

### 10. 사전 데이터 기록

```bash
adb shell '/data/local/tmp/stressapptest -M 1024 -s 1 -P OneZero256 -m 0 -i 0 -c 0 --printsec 1 --fill-preset zero --verify-after-fill --skip-final-check -l /data/local/tmp/sat-step10.log'
```

전체 영역을 `0x00`으로 기록한 후 OneZero256을 기록합니다. 비교 실행에서는 `--fill-preset none`, `zero`, `one`을 각각 사용합니다.

### 11. Runtime 시작 대기

```bash
adb shell '/data/local/tmp/stressapptest -M 1024 -s 600 -P OneZero256 -m 4 -i 4 -c 0 --printsec 10 --runtime-start-delay 10 --skip-final-check -l /data/local/tmp/sat-step11.log'
```

Valid·Empty 상태 구성이 끝난 후 10초를 기다리고 Runtime Worker를 시작합니다. 비교 실행에서는 `--runtime-start-delay` 값을 `0`, `1`, `10`, `30`으로 변경합니다.

### 12. Runtime Check만 실행

```bash
adb shell '/data/local/tmp/stressapptest -M 1024 -s 600 -P OneZero256 -m 0 -i 0 -c 4 --printsec 10 --skip-final-check -l /data/local/tmp/sat-step12.log'
```

Check Worker 4개가 Valid SAT 작업 단위를 반복해서 읽고 검사합니다. 확인할 로그 단계는 `worker:check, phase:runtime_check`입니다.

### 13. Runtime Copy와 destination 검사

```bash
adb shell '/data/local/tmp/stressapptest -M 1024 -s 600 -P OneZero256 -m 4 -i 0 -c 0 --printsec 10 --copy-verify-destination --skip-final-check -l /data/local/tmp/sat-step13.log'
```

Copy Worker 4개가 source read·checksum과 destination write를 수행합니다. 추가 readback은 `worker:copy, phase:destination_check`로 기록됩니다. Destination 검사 영향은 같은 명령에서 `--copy-verify-destination`만 제거한 조건과 비교합니다.

### 14. Runtime Invert만 실행

```bash
adb shell '/data/local/tmp/stressapptest -M 1024 -s 600 -P OneZero256 -m 0 -i 4 -c 0 --invert-range full --printsec 10 --skip-final-check -l /data/local/tmp/sat-step14.log'
```

Invert Worker 4개가 `precheck`, SAT 작업 단위 전체의 네 번 반전 저장과 `postcheck`를 수행합니다. 기존 범위 비교는 같은 명령에서 `--invert-range legacy`만 변경합니다.

### 15. 종료 시점 Valid 검사만 실행

```bash
adb shell '/data/local/tmp/stressapptest -M 1024 -s 1 -P OneZero256 -m 0 -i 0 -c 0 --printsec 1 --final-check-threads 1 -l /data/local/tmp/sat-step15.log'
```

Runtime의 시험 메모리 접근을 제거하고 Check Worker 1개가 수행하는 종료 시점 Valid 검사를 확인합니다. 기본 fine-lock queue와 `-M 1024` 조건에서는 Fill 이후 Valid로 설정된 616개 작업 단위를 검사합니다. 비교 실행에서는 `--final-check-threads` 값을 `1`, `2`, `4`, `8`로 변경합니다.

## 추가 옵션의 전체 실행 명령

다음 명령은 Pattern 배치, 종료 조건, DRAM 주파수와 주소 로그에 사용하는 추가 옵션의 전체 실행 예시입니다. DRAM 주파수 명령은 대상 시스템에 유효한 제어 경로와 write 권한이 있을 때 사용합니다. 기본 경로가 다른 환경에서는 `--ddr-node <경로>`를 추가합니다.

### 16. Pattern 위치 이동

```bash
adb shell '/data/local/tmp/stressapptest -M 1024 -s 1 -P OneZero256 -m 0 -i 0 -c 0 --printsec 1 --pattern-byte-offset 32 --verify-after-fill --skip-final-check -l /data/local/tmp/sat-step16.log'
```

SAT 작업 단위의 첫 주소에서 사용하는 OneZero256 Pattern index를 32 byte만큼 증가시킵니다. Pattern 데이터 생성과 expected checksum 계산에 같은 offset을 적용합니다. 비교 실행은 같은 명령에서 `--pattern-byte-offset 0`과 `32`만 변경합니다.

### 17. 여러 Pattern 순환 배정

```bash
adb shell '/data/local/tmp/stressapptest -M 1024 -s 600 -P OneZero256,FiveA256 -m 4 -i 4 -c 0 --printsec 10 --skip-final-check -l /data/local/tmp/sat-step17.log'
```

두 Pattern은 한 번의 Fill에서 SAT 작업 단위별로 순환 배정됩니다. Pattern 반환 순서는 입력 목록을 따르며, 각 Pattern이 배정되는 논리 주소는 queue 선택 순서와 Worker 실행 순서에 따라 결정됩니다. 배정된 Pattern 정보는 Copy·Invert·Check의 기대값 계산에 사용됩니다.

### 18. 첫 상세 mismatch 후 종료 요청

```bash
adb shell '/data/local/tmp/stressapptest -M 1024 -s 600 -P OneZero256 -m 4 -i 4 -c 0 --printsec 10 --fill-verify-every 1 --stop_on_errors --skip-final-check -l /data/local/tmp/sat-step18.log'
```

- Fill 중 상세 mismatch를 처리하면 종료 요청을 기록합니다.
- 현재 4 KiB 상세 검사와 SAT 작업 단위의 남은 검사에서 여러 mismatch 로그가 생성될 수 있습니다.
- 나머지 Fill은 즉시 검사를 추가하지 않고 완료하며, queue 구성 후 Runtime을 생략합니다.
- Runtime Memory Worker는 처리 중인 SAT 작업 단위를 queue에 반환한 뒤 종료 요청을 확인합니다. File, Network, Disk, Memory Region Worker도 현재 반복의 정리 지점에서 종료 요청을 확인합니다.
- 각 Worker는 종료 요청을 확인한 뒤 새 작업을 시작하지 않습니다.
- 메인 제어 반복문은 주파수 미사용·fixed 조건에서 최대 약 5초, sweep 조건에서 최대 약 1초 후 종료 요청을 확인하고 모든 Worker를 회수합니다.
- 종료 검사는 생략합니다.

비교 실행에서는 기준 명령의 `--stop_on_errors`를 제거합니다.

### 19. DRAM 주파수 고정

`<supported_frequency>`를 대상 제어 interface가 지원하는 값으로 교체합니다.

```bash
adb shell '/data/local/tmp/stressapptest -M 1024 -s 600 -P OneZero256 -m 4 -i 4 -c 0 --printsec 10 --ddr-freq <supported_frequency> --skip-final-check -l /data/local/tmp/sat-step19.log'
```

한 값은 초기 Fill 전에 한 번, Runtime 시작 직전에 한 번 전달됩니다. Mismatch 상세 로그의 `cur_mode`는 `fixed`로 기록됩니다. `cur_freq`와 `ddr_freq(...)`는 제어 경로에 성공적으로 전달한 요청값이며 실제 적용값은 대상 시스템 계측값으로 확인합니다.

### 20. 등록된 DRAM 주파수 전체 순환

```bash
adb shell '/data/local/tmp/stressapptest -M 1024 -s 600 -P OneZero256 -m 4 -i 4 -c 0 --printsec 10 --ddr-freq all --ddr-step 3 --skip-final-check -l /data/local/tmp/sat-step20.log'
```

Build에 등록된 값을 입력 순서대로 전환 요청합니다. 첫 값은 초기 Fill 전과 Runtime 시작 직전에 전달하고, Runtime에서는 두 번째 값부터 3초 간격으로 순환합니다. Fill, post-fill 검사와 queue 구성에서는 첫 값만 요청된 상태입니다. 초기 단계의 주파수 비교는 지원값 하나를 `--ddr-freq <supported_frequency>`로 고정한 별도 실행을 사용합니다. `DDR_FREQ write=...` 행은 제어 경로 write 시점이며 실제 적용 주파수는 대상 시스템 계측값으로 확인합니다.

쉼표 목록과 별도 제어 경로를 지정하는 전체 명령 형식은 다음과 같습니다. 두 주파수 placeholder와 `/path/to/ddr_control_node`를 대상 시스템에서 확인한 값으로 교체합니다.

```bash
adb shell '/data/local/tmp/stressapptest -M 1024 -s 600 -P OneZero256 -m 4 -i 4 -c 0 --printsec 10 --ddr-freq <supported_frequency_1>,<supported_frequency_2> --ddr-step 3 --ddr-node /path/to/ddr_control_node --skip-final-check -l /data/local/tmp/sat-step20-list.log'
```

### 21. 선택형 DRAM 주소 해석 프로필

```bash
adb shell '/data/local/tmp/stressapptest -M 1024 -s 600 -P OneZero256 -m 4 -i 4 -c 0 --printsec 10 --dram-map lpddr-v1 --skip-final-check -l /data/local/tmp/sat-step21.log'
```

오류 로그는 항상 `ch`, `rk`, `sc`, `bg`, `bank`, `row`, `col` 필드를 포함합니다. 물리 주소를 확인할 수 있고 프로필을 적용한 실행에서는 각 필드에 변환 결과를 기록합니다. 변환 조건을 충족하지 못하면 `unknown`을 기록합니다. 프로필 적용은 메모리 접근 순서를 변경하지 않습니다.

### 22. 단계별 논리 작업량 기록

```bash
adb shell '/data/local/tmp/stressapptest -M 1024 -s 600 -P OneZero256 -m 4 -i 4 -c 0 --invert-range full --diag-phase-summary --printsec 10 --skip-final-check -l /data/local/tmp/sat-step22.log'
```

시작 시 `DIAG_CONFIG`가 queue, Worker 수, Invert 범위, 종료 검사 mode와 DDR sweep 시작 단계를 출력합니다. `DIAG_CONFIG_OPTIONS`는 Fill 방향, 사전 접근, 검사, 대기, Copy destination 검사, strict·warm·tag mode, CPU stress 수, affinity, block history와 오류 로그 제한의 해석값을 출력합니다. 종료 시 `DIAG_SUMMARY`가 Fill, Copy와 Invert 단계의 block 수, 논리 read·write byte, 일반 memory word mismatch 수와 해당 phase의 첫 오류 기록 시점·DDR 요청 epoch를 출력합니다. Tag mismatch는 Tag 상세 로그와 전체 error count로 집계됩니다. 일반 memory mismatch는 첫 word 차이에서 기록하고 대체 경로는 작업 단위 집계 시점에 기록합니다. `first_error_worker_bytes`는 최초 오류를 보고한 Worker가 같은 phase에서 오류 작업 단위를 시작하기 전에 완료한 논리 byte입니다. 비교 실행은 두 조건 모두 `--diag-phase-summary`를 사용합니다. 오류 수는 처리 block 또는 논리 read byte 기준으로 정규화합니다.

### 23. 메모리 할당과 first-touch 상태 기록

```bash
adb shell '/data/local/tmp/stressapptest -M 1024 -s 1 -P OneZero256 -m 0 -i 0 -c 0 --verify-after-fill --diag-vm-stats --skip-final-check -l /data/local/tmp/sat-step23.log'
```

`DIAG_VM`에서 allocation, initial Fill과 post-fill 검사 사이의 minor·major fault delta, RSS와 mapping 방식을 확인합니다. `--prefault-pages` 영향은 같은 명령에서 해당 옵션 하나만 추가하여 비교합니다.

### 24. 마지막 추적 write 이력과 상세 로그 예산

```bash
adb shell '/data/local/tmp/stressapptest -M 1024 -s 600 -P OneZero256 -m 4 -i 4 -c 0 --invert-range full --diag-block-history --error-log-limit 1 --printsec 10 --skip-final-check -l /data/local/tmp/sat-step24.log'
```

Mismatch 상세 로그 한 개에 `block_history(...)`가 추가됩니다. 이후 상세 mismatch 문자열은 생략하며 ErrorDiagnoser 보고, 오류 집계, reread와 expected 복구는 계속 수행합니다. 종료 시 `DIAG_ERROR_LOG`에서 실제 상세 출력 수와 생략 수를 확인합니다. Block history 비용을 사용하고 출력도 받으려면 `--error-log-limit`에 1 이상의 값을 사용합니다.

## 단계별 결과 해석

| 최초 검출 단계 | 확인되는 실행 구간 |
|---|---|
| `fill/immediate_check` | 개별 SAT 작업 단위의 Pattern write 직후 첫 readback |
| `post_fill/full_check` | 전체 Fill 완료부터 post-fill 전체 검사까지의 구간 |
| `check/runtime_check` | Runtime의 반복 read와 checksum 구간 |
| `copy/source_check` | Runtime Copy source read 구간 |
| `copy/destination_check` | Runtime Copy destination write 직후 readback 구간 |
| `invert/precheck` | Invert가 선택한 작업 단위의 반전 전 상태 |
| `invert/postcheck` | 네 번의 반전 저장과 마지막 검사 구간 |
| `check/final_check` | Runtime 종료부터 Valid queue 검사까지의 구간 |

`phase`는 오류를 처음 검출한 소프트웨어 검사 위치입니다. 원인 위치는 cache·interconnect·memory controller·PHY·DRAM 계측 결과를 함께 사용하여 판정합니다.

## 권장 비교 프로세스

1. Pattern 하나를 `-P`에 지정하고 실행마다 새 프로세스를 시작합니다.
2. 기준 실행에 `--invert-range legacy` 또는 `full`을 명시합니다. 기존 동작 비교에는 `legacy`를 사용합니다.
3. `--fill-verify-every 1`과 `--verify-after-fill`을 각각 별도 실행하여 검출 단계를 구분합니다.
4. `--prefault-pages`, `--fill-threads`, `--fill-direction`, `--fill-yield-bytes`를 한 항목씩 변경합니다.
5. Runtime은 `-c`, `-m`, `-i` 중 한 Worker 종류만 활성화한 실행으로 분리합니다.
6. 주파수 비교는 지원되는 값 하나를 고정한 실행부터 수행합니다. Sweep은 고정 조건 결과를 확보한 후 사용합니다.
7. `--diag-phase-summary`를 모든 비교 조건에 적용하고 `DIAG_CONFIG`와 `DIAG_CONFIG_OPTIONS`의 해석 결과를 확인합니다. 처리 block 또는 논리 read byte당 mismatch를 계산합니다.
8. `--diag-block-history`는 1차 조건 분리 후 오류가 발생한 조건의 2차 위치 분석에 사용합니다.
9. 시작 온도, 전원 상태, CPU affinity, 백그라운드 부하, 로그 파일 사용 여부와 반복 횟수를 동일하게 유지합니다.

다중 `-P` 목록은 한 실행의 SAT 작업 단위에 Pattern을 혼합합니다. Pattern별 비교 결과에는 각 Pattern을 별도 프로세스로 실행한 로그를 사용합니다.

## 공개 옵션의 적용 범위

검사 옵션은 운영체제가 제공한 일반 cacheable mapping을 CPU load로 읽습니다. Invert Worker의 `FastFlushHint()`는 AArch64에서 `dc cvau`를 사용하며, Fill·Copy·Check 경로는 각 store 또는 load마다 강제 cache eviction을 수행하지 않습니다. Mismatch 상세 처리에서는 해당 64-bit 위치를 expected로 복구하고, 물리 페이지 배치와 DRAM row 선택은 kernel과 memory-controller 설정을 따릅니다.

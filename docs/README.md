# 실행 원리와 읽기 순서

이 매뉴얼은 Android ARM64 환경에서 stressapptest가 시험 메모리를 준비하고, Worker를 실행하고, 데이터 오류를 검사하는 순서를 설명합니다.

[Android ARM64 최신 실행 파일 다운로드](https://github.com/stpcoder/stressapptest/releases/latest/download/stressapptest-android-arm64)

## 기준 실행 명령

다음 명령을 기준으로 전체 동작을 확인합니다.

```bash
adb shell '/data/local/tmp/stressapptest -M 1024 -s 600 -P OneZero256 -m 4 -i 4 -c 0 --printsec 10'
```

| 옵션 | 설정 내용 |
|---|---|
| `-M 1024` | 1,024 MiB 시험 메모리 할당 |
| `-s 600` | Runtime Worker를 600초 동안 실행 |
| `-P OneZero256` | 모든 초기 SAT 작업 단위에 OneZero256 Pattern 배정 |
| `-m 4` | Copy Worker 4개 실행 |
| `-i 4` | Invert Worker 4개 실행 |
| `-c 0` | Runtime Check Worker를 생성하지 않음 |
| `--printsec 10` | 진행 상태를 10초 간격으로 출력 |

`-s`는 Runtime Worker 실행 시간에 적용됩니다. 초기 Fill과 종료 시점의 Valid 검사는 이 시간의 앞뒤에서 실행됩니다.

## 기준 명령의 실행 순서

```text
1,024 MiB 메모리 할당
  → Pattern과 기대 checksum 생성
  → Fill Worker 8개가 전체 영역에 OneZero256 기록
  → SAT 작업 단위를 Valid·Empty 상태로 구성
  → Copy Worker 4개와 Invert Worker 4개를 600초 동안 실행
  → Runtime Worker 종료
  → Check Worker 8개가 종료 시점의 Valid 작업 단위를 검사
  → 결과 출력과 메모리 해제
```

기본 SAT 작업 단위는 1 MiB이므로 `-M 1024`는 1,024개의 작업 단위를 만듭니다. 초기 Fill은 1,024 MiB 전체를 기록합니다. 기본 fine-lock queue는 Runtime 시작 전에 408개를 Empty, 616개를 Valid 상태로 설정합니다. Empty 전환은 Pattern 상태를 해제하며 메모리 할당과 기록된 byte는 유지됩니다.

<sub><em>Valid: 기대 Pattern 정보를 보유하여 읽기 원본과 검사 대상으로 사용할 수 있는 상태입니다.</em></sub>
<sub><em>Empty: Copy Worker가 새 데이터를 기록할 대상으로 사용할 수 있는 상태입니다.</em></sub>

## OneZero256 데이터 구성

OneZero256은 다음 64 byte 배열을 반복합니다.

```text
offset  0–31 : 0x00 32 byte
offset 32–63 : 0xFF 32 byte
```

`FillPage()`는 한 번의 store로 64-bit(8 byte)를 기록합니다. `0x00` 8 byte를 네 번, `0xFF` 8 byte를 네 번 기록하여 64 byte 배열을 구성합니다. 이 배열을 1 MiB 작업 단위 전체에 반복합니다.

## Worker 종류와 동작

### Copy Worker 4개

각 Copy Worker는 다음 순서를 반복합니다.

```text
Valid 원본 선택
  → Empty 대상 선택
  → 원본 read와 checksum 계산
  → 대상 write
  → 대상을 Valid로 변경
  → 기존 원본을 Empty로 변경
```

기본 Copy 검사는 원본을 읽으면서 계산한 checksum을 확인합니다. `--copy-verify-destination`은 대상 기록 직후 readback 검사를 추가합니다.

### Invert Worker 4개

각 Invert Worker는 Valid 작업 단위 하나를 선택하여 다음 순서로 처리합니다.

```text
반전 전 checksum 검사
  → 낮은 주소에서 높은 주소 방향으로 선택 범위 bit 반전 후 저장
  → 높은 주소에서 낮은 주소 방향으로 선택 범위 bit 반전 후 저장
  → 높은 주소에서 낮은 주소 방향으로 선택 범위 bit 반전 후 저장
  → 낮은 주소에서 높은 주소 방향으로 선택 범위 bit 반전 후 저장
  → 반전 후 checksum 검사
```

선택 범위의 각 위치를 네 번 반전하므로 마지막 데이터는 시작 Pattern으로 복원됩니다. 기본 `legacy` 범위는 기존 공개 코드의 동작을 유지하며, `--invert-range full`은 SAT 작업 단위 전체를 처리합니다. 각 반전은 같은 주소를 읽고, CPU에서 bitwise NOT을 계산하고, 같은 주소에 저장하는 read-modify-write 작업입니다.

### 종료 시점 Check Worker

기본 동작에서 Runtime Check Worker는 종료 요청 이후 Valid queue를 끝까지 검사합니다. 남은 항목은 Fill Worker 수와 같은 수의 보조 Check Worker가 검사합니다. `--final-check-threads N`을 명시하면 Runtime Check Worker가 종료 drain을 수행하지 않고 N개의 별도 Worker가 검사를 담당합니다. `--skip-final-check`는 종료 검사를 생략합니다.

## 오류 검사와 로그 출력

Worker가 계산한 checksum과 기대 checksum이 다르면 다음 처리를 수행합니다.

```text
4 KiB 구간 checksum mismatch
  → 64-bit 단위로 실제값과 기대값 비교
  → mismatch 주소를 다시 읽음
  → read·reread·expected와 검출 단계를 로그에 기록
  → 해당 64-bit 값을 expected로 복구
```

`worker`와 `phase`는 오류를 처음 검출한 소프트웨어 위치를 표시합니다. `sat_block`, `sat_offset`, `block_offset`은 시험 영역 내부의 논리적 위치를 표시합니다. 물리 주소와 DRAM 좌표는 실행 권한과 선택한 주소 변환 프로필에 따라 출력됩니다.

## Cache와 LPDDR 접근

Android ARM64 실행 경로는 일반 cacheable load와 store를 사용합니다. CPU는 가상 주소로 명령을 실행하며 MMU가 물리 주소 변환을 수행합니다. Cache miss는 하위 cache 또는 메모리 계층의 read 요청을 만들고, 수정된 cache line의 write-back은 하위 계층의 write 요청을 만듭니다.

`-M 1024 -m 4 -i 4`는 cache 용량보다 큰 영역을 8개 Runtime Worker가 반복 처리합니다. 이 접근은 cache refill, dirty line 교체와 interconnect 전송을 발생시킬 수 있습니다. 실제 memory-controller 요청량은 DMC 계측값으로 확인합니다.

Copy Worker 4개와 Invert Worker 4개는 DRAM die에 고정되지 않습니다. 각 Worker는 queue에서 잠긴 1 MiB 작업 단위를 선택합니다. 물리 페이지 배치와 channel·rank·bank 선택은 kernel의 page allocation과 memory-controller 주소 해석에 따라 결정됩니다.

오류 처리의 reread는 현재 cache 상태에서 같은 가상 주소를 한 번 더 CPU load합니다. 해당 load의 DRAM 도달 여부는 PMU 또는 memory-controller 계측값으로 확인합니다.

<sub><em>Write-back: 수정된 cache line을 하위 cache 또는 메모리 계층으로 전달하는 동작입니다.</em></sub>
<sub><em>Readback: write가 끝난 영역을 다시 읽어 기대 데이터와 비교하는 검사입니다.</em></sub>

## 권장 읽기 순서

1. [실행 순서 한눈에 보기](02-execution-flow.md)
2. [메모리 Worker 종류와 동작](07-memory-workers.md)
3. [메모리를 복사하고 오류를 찾는 과정](09-copy-and-verification.md)
4. [Cache에서 LPDDR까지 데이터가 이동하는 과정](04-cache-and-arm64.md)
5. [단계별 오류 검출과 옵션 영향 분석](18-stage-debugging-and-option-risk.md)

전체 옵션은 [명령행 옵션 정리](10-all-options.md), 소스 위치는 [소스 코드 찾아보기](15-source-map.md), 기술 용어는 [용어 설명](16-glossary.md)에서 확인할 수 있습니다.

## 안전 확인

첫 실행은 작은 `-M`과 `-s`로 시작하여 Android 시스템 프로세스용 메모리를 확보합니다. Raw block device에 쓰는 `-d --destructive`는 데이터 삭제가 허용된 시험 장치에서만 사용합니다.

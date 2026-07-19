# 메모리를 복사하고 오류를 찾는 과정

기본 복사 방식은 원본을 읽으면서 checksum을 계산하고 같은 반복문에서 대상에 씁니다. 원본의 checksum이 기대값과 다르면 해당 구간을 다시 자세히 비교합니다. 대상에 쓴 데이터는 이후 원본으로 선택되거나 마지막 전체 검사를 수행할 때 확인합니다.

## 검증에 사용하는 checksum

`CrcCopyPage`와 `CrcCheckPage`가 사용하는 checksum은 `src/adler32memcpy.cc`에 구현된 modified Adler 방식입니다. 함수 이름에 `Crc`가 포함되어 있지만 CRC polynomial 연산은 사용하지 않습니다.

32-bit 데이터 word를 네 개의 누산기에 번갈아 반영합니다.

```text
a1, a2: 데이터 값을 더하는 누산기
b1, b2: a 누산기의 값을 다시 더하는 누산기
```

이 checksum은 반복 pattern의 데이터 변화를 빠르게 찾는 용도로 사용합니다. 암호학적 checksum과 같은 충돌 방지 성능이나 CRC polynomial 방식의 오류 검출 특성을 제공하지는 않습니다.

<sub><em>Modified Adler checksum: 네 개의 누산기로 32-bit data word와 누적합을 계산하는 stressapptest의 고속 검증값입니다.</em></sub><br>
<sub><em>Collision: 서로 다른 데이터가 동일한 checksum 결과를 생성하는 경우입니다.</em></sub>

## 기대 checksum 만들기

각 `Pattern` 객체를 초기화할 때 4 KiB pattern의 checksum을 계산하여 `Pattern::crc_`에 저장합니다.

> **파일:** `src/pattern.cc` · **함수:** `Pattern::CalculateCrc()` · **기준:** `73b9df2`

```cpp
uint64 a1 = 1;
uint64 a2 = 1;
uint64 b1 = 0;
uint64 b2 = 0;
int blocksize = 4096;
int i = 0;
int count = blocksize / sizeof i;
while (i < count) {
  a1 += pattern(i);
  b1 += a1;
  i++;
  a1 += pattern(i);
  b1 += a1;
  i++;

  a2 += pattern(i);
  b2 += a2;
  i++;
  a2 += pattern(i);
  b2 += a2;
  i++;
}
crc_->Set(a1, a2, b1, b2);
```

**코드 설명:** 네 누산기에 pattern으로 만든 4 KiB의 32-bit word를 순서대로 반영합니다. 이 값은 실제 메모리를 읽은 결과가 아니라, 이후 검사에 사용할 기대값입니다.

기본 1 MiB SAT block은 4 KiB 검사 구간 256개로 나뉩니다. Pattern이 주기적으로 반복되므로 각 구간을 같은 기대 checksum과 비교합니다.

## 복사 중 실제 읽기·쓰기 순서

`CrcCopyPage(destination, source)`는 block을 4 KiB씩 나누어 `AdlerMemcpyC()`를 호출합니다.

반복문 안에서는 다음 순서로 처리합니다.

```text
원본 word 읽기
 → checksum 누산기 갱신
 → 대상 word 쓰기
 → 다음 word 처리
```

4 KiB 처리가 끝날 때마다 원본에서 계산한 checksum을 기대 pattern의 checksum과 비교합니다.

> **파일:** `src/worker.cc` · **함수:** `WorkerThread::CrcCopyPage()` · **기준:** `73b9df2`

```cpp
for (int currentblock = 0; currentblock < blocks; currentblock++) {
  uint64 *targetmem = targetmembase + currentblock * blockwords;
  uint64 *sourcemem = sourcemembase + currentblock * blockwords;

  AdlerChecksum crc;
  if (tag_mode_) {
    AdlerAddrMemcpyC(targetmem, sourcemem, blocksize, &crc, srcpe);
  } else {
    AdlerMemcpyC(targetmem, sourcemem, blocksize, &crc);
  }

  if (!crc.Equals(*expectedcrc)) {
    int errorcount = CheckRegion(sourcemem, srcpe->pattern,
                                 srcpe->lastcpu, 4096,
                                 currentblock * 4096, 0);
    errors += errorcount;
  }
}
dstpe->pattern = srcpe->pattern;
dstpe->lastcpu = sched_getcpu();
```

**코드 설명:** 1 MiB SAT block을 4 KiB씩 처리합니다. `AdlerMemcpyC()`는 원본 읽기, checksum 계산, 대상 쓰기를 하나의 반복문에서 수행합니다. Checksum이 기대값과 다르면 같은 원본 구간을 word 단위로 다시 비교합니다.

중요한 점:

- 원본은 복사와 동시에 검사합니다.
- 대상에 쓴 데이터는 해당 block을 이후 원본으로 사용하거나 마지막 전체 검사를 수행할 때 검사합니다.
- 잘못된 원본 데이터가 대상에 복사될 수 있으므로 오류 데이터를 복구하는 경로가 있습니다.

## 대상 block에 쓴 데이터를 검사하는 시점

대상 block에 쓴 데이터의 오류는 다음 시점에 발견할 수 있습니다.

1. 대상 block이 나중에 `CopyThread`의 원본으로 선택될 때
2. `CheckThread`가 해당 block을 검사할 때
3. 파일·네트워크 Worker가 원본으로 사용하기 전에 검사할 때
4. 설정한 시험 시간이 끝난 뒤 마지막 전체 검사를 수행할 때

따라서 실제 오류가 발생한 시각과 로그에 오류가 기록된 시각은 다를 수 있습니다.

## Checksum 불일치가 발생했을 때의 재검사

Checksum이 다르면 `CheckRegion()`이 해당 4 KiB 구간을 64-bit word 단위로 다시 읽습니다.

```text
실제 64-bit 값 읽기
기대 pattern의 32-bit word 두 개를 64-bit 값으로 조합
실제값과 기대값 비교
```

먼저 최대 128개의 상세 오류를 저장합니다. 불일치가 더 많으면 page 또는 block 단위 오류로 추가 처리합니다.

## 일시적인 원본 읽기 오류 확인

첫 checksum은 다르지만 자세한 재검사에서 word 불일치가 발견되지 않으면 일시적인 원본 읽기 오류 처리 경로를 실행합니다.

이 결과는 첫 번째 읽기와 두 번째 읽기의 값이 달랐을 가능성을 의미합니다. 대상에는 첫 복사에서 읽은 값이 저장되어 있으므로 다음 절차로 다시 확인합니다.

```text
첫 복사 때 대상에 저장된 데이터
 → 원본에 다시 복사
 → 원본을 기대 pattern과 word 단위로 비교
```

이 과정은 일시적으로 잘못 읽힌 값을 메모리에 다시 기록하여 어느 주소의 값이 달랐는지 확인하기 위한 것입니다.

## 오류가 발생한 위치의 재검사와 복구

`ProcessError()`는 다음 정보를 기록합니다.

- 현재 검사를 수행한 CPU
- 마지막으로 데이터를 쓴 CPU
- virtual address
- 계산 가능한 경우 physical address
- 실제값
- 기대값
- 다시 읽은 값
- pattern name
- 옵션을 설정한 경우 DIMM·channel 추정값

그 후 잘못된 word를 기대값으로 복구하여 오류 데이터가 다음 복사 작업으로 계속 전달되지 않게 합니다.

공통 ARM64 구현에서는 다시 읽기 전에 호출하는 cache flush가 실제 cache 관리 명령을 실행하지 않을 수 있습니다. 실제값과 재검사값의 차이로 읽기 오류와 쓰기 오류를 분류할 때에는 `has_clflush_` 상태와 `Flush()` 실행 여부를 함께 확인해야 합니다.

## 다른 주소의 데이터가 읽힌 경우

`--tag_mode`에서는 각 64 B cache line의 첫 8 B에 virtual address로 만든 tag를 기록합니다. `CheckRegion()`은 첫 8 B의 기대값을 현재 주소로 계산하고, 나머지 word는 pattern 값과 비교합니다.

다른 주소의 line이 잘못 들어오면:

```text
데이터 pattern 일부는 우연히 일치할 수 있음
주소 tag는 현재 대상 주소와 일치하지 않음
```

따라서 다른 주소의 데이터가 전달된 오류를 더 직접적으로 확인할 수 있습니다.

## `-F`의 검증 범위

`-F`는 `strict_ = false`로 설정하여 복사 중 즉시 검사를 줄입니다.

꺼지는 항목:

- `CopyThread`가 복사할 때 수행하는 원본 checksum
- `InvertThread`가 반전 작업 전후에 수행하는 checksum
- 파일·네트워크 Worker가 원본과 대상을 즉시 검사하는 checksum

유지되는 항목:

- Pattern 상태 정보 전달
- 설정한 시험 시간 종료 후 `CheckThread`의 마지막 전체 검사
- Sector·network protocol 자체의 일부 오류 검사

즉, `-F`는 복사 작업 중의 원본·대상 checksum 검사를 생략하지만 마지막 전체 검사는 유지합니다.

## 오류 검증 기능을 시험하는 옵션

- `--force_errors`: `CopyThread`가 낮은 확률로 원본 byte를 `0xba`로 변경하여 오류 처리 코드를 실행합니다.
- `--force_errors_like_crazy`: 위 기능에 더하여 약 10초마다 valid block의 pattern 정보를 바꾸어 반복적으로 오류를 만듭니다.

이 옵션은 오류 보고 기능을 시험하기 위해 프로그램이 의도적으로 데이터 또는 상태 정보를 변경합니다. 메모리 접근 방식과 hardware 부하 강도는 Worker 관련 옵션으로 별도로 결정됩니다.

<sub><em>Error injection: 오류 처리 경로를 시험하기 위해 software가 의도적으로 data 또는 metadata를 변경하는 기능입니다.</em></sub>

## 조기 종료 조건

- `--stop_on_errors`: 지원되는 오류 처리 경로에서 첫 오류가 발생하면 즉시 프로그램을 종료합니다.
- `--max_errors N`: 전체 오류 수가 N을 초과하면 주 실행 반복을 조기에 끝냅니다.

현재 주 실행 반복문의 종료 조건은 `errors > max_errorcount_`입니다. 따라서 누적 오류 수가 N과 같을 때가 아니라 N을 초과했을 때 종료 절차가 시작됩니다.

## 출력되는 처리량 해석

Worker는 처리한 block 수와 block 크기를 사용하여 논리적 처리량을 계산합니다.

| Worker | 프로그램이 보고하는 메모리 처리량 |
|---|---|
| Copy | block × 2 |
| Check | block × 1 |
| Invert | 내부 처리 block 수에 추가 배수 적용 |
| File | 메모리와 저장 장치를 별도로 계산 |
| Network | 송신과 수신을 합하여 장치 처리량 × 2 |

논리적 처리량에는 checksum 재검사, write allocate, cache refill·write-back, prefetch, queue 상태 정보, filesystem·socket 내부 복사가 직접 반영되지 않습니다. LPDDR bandwidth는 DMC counter로 측정해야 합니다.

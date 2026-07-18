# copy, checksum, 오류 검증

## `CRC`라는 이름의 실제 의미

소스 함수명 `CrcCopyPage`와 `CrcCheckPage`의 checksum 구현은 `src/adler32memcpy.cc`에 정의된 modified Adler 방식이다. CRC polynomial 연산은 사용하지 않는다.

32-bit data word를 번갈아 네 누산기에 반영한다.

```text
a1, a2: data 누적
b1, b2: a 누적값의 누적
```

이 checksum은 반복 pattern의 고속 corruption screening에 사용된다. cryptographic collision resistance와 CRC polynomial 기반 검출 특성은 제공하지 않는다.

<sub><em>Modified Adler checksum: 네 개의 누산기로 32-bit data word와 누적합을 계산하는 stressapptest의 고속 검증값입니다.</em></sub><br>
<sub><em>Collision: 서로 다른 데이터가 동일한 checksum 결과를 생성하는 경우입니다.</em></sub>

## Expected checksum 생성

Pattern variant가 초기화될 때 첫 4 KiB pattern의 checksum을 계산해 `Pattern::crc_`에 저장한다.

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

**해석:** 네 누산기는 pattern에서 생성한 4 KiB의 32-bit word를 순서대로 반영합니다. 이 값은 실제 memory를 읽은 결과가 아니며 각 Pattern object의 기대값입니다.

SAT block 기본 1 MiB는 4 KiB slice 256개다. Pattern이 주기적으로 반복되므로 각 slice를 같은 expected checksum과 비교한다.

## 기본 copy의 정확한 read/write 순서

`CrcCopyPage(destination, source)`는 4 KiB마다 `AdlerMemcpyC()`를 호출한다.

개념적 inner loop:

```text
source word load
 → checksum a/b update
 → destination word store
 → next word
```

4 KiB가 끝나면 계산한 source checksum을 expected pattern checksum과 비교한다.

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

**해석:** 1 MiB SAT block은 4 KiB slice 단위로 처리됩니다. `AdlerMemcpyC()`가 source load, checksum update, destination store를 한 loop에서 수행합니다. checksum mismatch가 발생하면 같은 source slice를 word 단위로 다시 비교합니다.

중요한 점:

- source는 copy하면서 즉시 검증됨
- destination store 검증은 이후 source 선택 또는 final check에서 수행
- source corruption이 destination으로 복사될 수 있으므로 mismatch 복구 경로가 존재

## Destination write는 언제 검증되는가?

destination write 오류는 다음 시점 중 하나에서 검출될 수 있다.

1. destination block이 나중에 CopyThread source로 선택될 때
2. CheckThread가 block을 검사할 때
3. File/Network worker가 source로 사용하기 전 strict check할 때
4. timed run 종료 후 final full check할 때

따라서 오류 발생 시각과 log 검출 시각이 다를 수 있다.

## Checksum mismatch 후 slow compare

checksum이 다르면 `CheckRegion()`이 해당 4 KiB를 64-bit word 단위로 다시 읽는다.

```text
actual 64-bit load
expected pattern word 두 개 조합
actual != expected 비교
```

최대 128개의 error record를 우선 저장하고, 더 많은 mismatch가 있으면 page/block error 형태로 추가 처리한다.

## Transient source read 오류 포착

checksum mismatch 이후 slow reread에서 word mismatch가 확인되지 않는 경우에는 transient source read 오류 처리 경로가 실행된다.

이 조건은 첫 read와 두 번째 read의 결과가 달랐을 가능성을 나타낸다. destination에는 첫 copy에서 읽은 데이터가 저장되어 있으므로 코드는 다음 절차를 수행한다.

```text
첫 copy 때 destination에 저장된 captured data
 → source에 다시 memcpy
 → source를 expected pattern과 slow compare
```

이 방식으로 일시적인 read-path corruption을 재현 가능한 memory contents로 바꾸어 상세 주소를 찾으려 한다.

## 오류 reread와 수정

`ProcessError()`는 다음 정보 field를 기록한다.

- current CPU
- last writer CPU
- virtual address
- possible physical address
- actual value
- expected value
- reread value
- pattern name
- 설정된 경우 DIMM/channel 추정

그 후 잘못된 word를 expected 값으로 덮어써 corruption이 다음 copy에 계속 전파되지 않게 한다.

generic ARM64에서는 reread 전 cache flush가 no-op일 수 있다. actual과 reread 비교에 따른 read/write error 분류에는 `has_clflush_` 상태와 `Flush()` 실행 여부를 함께 기록한다.

## Tag mismatch

`--tag_mode`에서는 각 64 B line 첫 8 B가 virtual-address-derived tag다. CheckRegion은 해당 위치의 expected value 계산에 현재 address tag를 사용하고 나머지 word에는 pattern 값을 사용한다.

다른 주소의 line이 잘못 들어오면:

```text
data pattern 일부는 우연히 맞을 수 있음
address tag는 현재 destination 주소와 불일치
```

하여 wrong-address 계열 오류를 더 직접적으로 보고할 수 있다.

## `-F`의 검증 범위

`-F`는 `strict_ = false`로 만든다.

꺼지는 항목:

- CopyThread의 per-transaction source checksum
- InvertThread 전/후 checksum
- File/Network source/destination의 즉시 checksum

유지되는 항목:

- pattern metadata 이동
- timed run 후 final CheckThread 전체 검사
- sector/network의 별도 protocol/error handling 일부

`-F`는 transaction 중 source/destination checksum 검사를 비활성화하고 timed run 이후 final CheckThread 검사는 유지한다.

## Error injection option

- `--force_errors`: CopyThread가 낮은 확률로 source byte를 `0xba`로 변경하고 여러 error path도 자극
- `--force_errors_like_crazy`: 위 기능을 켜고 run loop가 약 10초마다 valid block의 pattern metadata를 바꾸어 반복 오류 생성

이 option은 error reporting code를 검증하기 위한 software corruption injection을 수행한다. memory access pattern과 hardware transaction 강도는 별도의 worker option으로 결정된다.

<sub><em>Error injection: 오류 처리 경로를 시험하기 위해 software가 의도적으로 data 또는 metadata를 변경하는 기능입니다.</em></sub>

## 조기 종료 조건

- `--stop_on_errors`: 일부 error path에서 첫 오류 즉시 process exit 또는 stop
- `--max_errors N`: total error count가 threshold를 넘으면 main loop 조기 종료

현재 main loop의 종료 조건은 `errors > max_errorcount_`이다. `--max_errors N`을 지정하면 누적 오류 수가 N을 초과한 검사 시점에 종료 절차가 시작된다.

## Bandwidth 통계 해석

worker는 처리 block 수와 block size를 곱해 논리적 byte를 계산한다.

| Worker | 보고되는 memory data |
|---|---|
| Copy | block × 2 |
| Check | block × 1 |
| Invert | 내부 page count에 추가 multiplier |
| File | memory/device 각각 별도 multiplier |
| Network | send+receive 기준 device × 2 |

논리적 byte 계산에는 checksum reread, write allocate, cache refill/write-back, prefetch, queue metadata 및 filesystem/socket 내부 copy가 직접 반영되지 않는다. LPDDR bandwidth 분석에는 DMC counter 측정값을 사용한다.

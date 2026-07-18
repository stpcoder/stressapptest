# copy, checksum, 오류 검증

## `CRC`라는 이름의 실제 의미

소스 함수 이름은 `CrcCopyPage`, `CrcCheckPage`이지만 표준 CRC polynomial을 구현하지 않는다. `src/adler32memcpy.cc`의 modified Adler 방식이다.

32-bit data word를 번갈아 네 누산기에 반영한다.

```text
a1, a2: data 누적
b1, b2: a 누적값의 누적
```

cryptographic hash가 아니며 collision이 불가능한 것도 아니다. 반복 pattern의 고속 corruption screening을 위한 checksum이다.

## Expected checksum 생성

Pattern variant가 초기화될 때 첫 4 KiB pattern의 checksum을 계산해 `Pattern::crc_`에 저장한다.

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

중요한 점:

- source는 copy하면서 즉시 검증됨
- destination은 store되지만 같은 loop에서 다시 읽어 검증하지 않음
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

흥미로운 경로는 checksum mismatch가 났지만 slow reread에서 mismatch가 보이지 않는 경우다.

이는 첫 read에서 transient corruption을 보았고 두 번째 read에서는 정상값을 보았을 가능성이 있다. 원래 copy된 destination에는 첫 read 값이 남아 있으므로 코드가 다음을 수행한다.

```text
첫 copy 때 destination에 저장된 captured data
 → source에 다시 memcpy
 → source를 expected pattern과 slow compare
```

이 방식으로 일시적인 read-path corruption을 재현 가능한 memory contents로 바꾸어 상세 주소를 찾으려 한다.

## 오류 reread와 수정

`ProcessError()`는 대략 다음 정보를 기록한다.

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

generic ARM64에서는 reread 전 cache flush가 no-op일 수 있으므로 actual과 reread 비교에 따른 read/write error 분류를 절대적인 진단으로 사용하면 안 된다.

## Tag mismatch

`--tag_mode`에서는 각 64 B line 첫 8 B가 virtual-address-derived tag다. CheckRegion은 해당 위치의 expected value를 pattern 대신 현재 address tag로 계산한다.

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

따라서 `-F`는 “검증 완전 비활성화”가 아니라 “이동 중 즉시 검증 생략”에 가깝다.

## Error injection option

- `--force_errors`: CopyThread가 낮은 확률로 source byte를 `0xba`로 변경하고 여러 error path도 자극
- `--force_errors_like_crazy`: 위 기능을 켜고 run loop가 약 10초마다 valid block의 pattern metadata를 바꾸어 반복 오류 생성

이는 hardware stress 강화 option이 아니라 error reporting code를 검증하기 위한 software injection이다.

## 조기 종료 조건

- `--stop_on_errors`: 일부 error path에서 첫 오류 즉시 process exit 또는 stop
- `--max_errors N`: total error count가 threshold를 넘으면 main loop 조기 종료

현재 main loop 비교는 `errors > max_errorcount_`이므로 정확히 N개가 되는 순간이 아니라 N개를 초과한 뒤 종료될 수 있다.

## Bandwidth 통계 해석

worker는 처리 block 수와 block size를 곱해 논리적 byte를 계산한다.

| Worker | 보고되는 memory data |
|---|---|
| Copy | block × 2 |
| Check | block × 1 |
| Invert | 내부 page count에 추가 multiplier |
| File | memory/device 각각 별도 multiplier |
| Network | send+receive 기준 device × 2 |

checksum reread, write allocate, cache refill/writeback, prefetch, queue metadata, filesystem/socket copy는 이 단순 계산과 다를 수 있다. LPDDR bandwidth로 사용할 때는 DMC counter로 교차 검증해야 한다.

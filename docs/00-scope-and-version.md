# 분석 기준과 버전

## 이 문서가 분석한 소스

이 문서는 다음 소스 코드를 직접 확인하여 작성했습니다.

| 항목 | 값 |
|---|---|
| 원본 저장소 | `stressapptest/stressapptest` |
| Branch | `master` |
| Commit | `73b9df227e89cd52b09852056843610722b7b7ae` |
| Version 표기 | `v1.0.11-2-g73b9df2` |
| 주요 실행 환경 | Android·Linux AArch64, 모바일 LPDDR SoC |
| 기본 SAT block | 1 MiB |
| 코드 cache-line 상수 | 64 B |

문서의 코드 위치와 구현 설명은 위 commit을 기준으로 합니다. 원본 저장소가 변경되면 명령행 옵션 처리, ARM assembly, 기본값을 다시 확인해야 합니다.

## 코드에서 확인한 기본 크기

> **파일:** `src/sattypes.h` · **구간:** 공통 상수 · **기준:** `73b9df2`

```cpp
static const int kMegabyte = (1024LL*1024LL);
static const int kSatDiskPage = 8;
static const int kSatPageSize = (1024LL*1024LL);
static const int kCacheLineSize = 64;
static const uint16_t kNetworkPort = 19996;
```

**코드 설명:** 기본 SAT block은 1 MiB이고 파일 I/O는 한 번에 8개 block을 처리합니다. 코드에서 사용하는 cache line 크기 상수는 64 B이며 네트워크 시험에는 port 19996을 사용합니다. `kCacheLineSize`는 실행 중 SoC의 cache line을 확인한 값이 아니라 compile 시 정해지는 일부 처리 코드의 단위입니다.

## GitHub master와 AOSP mirror의 버전 관계

현재 분석한 GitHub master에서는 Android 전용 build 파일이 제거되었습니다. Android Open Source Project는 별도의 `platform/external/stressapptest` mirror와 `Android.bp`를 유지합니다.

두 저장소는 서로 다른 commit 이력을 가집니다. 분석과 시험에는 실제 실행 파일을 만든 저장소와 commit을 적용해야 합니다. 확인한 차이는 다음과 같습니다.

- GitHub master에는 ARM64 NEON `AdlerMemcpyAsm()` 구현이 있습니다.
- AOSP mirror는 `Android.bp`로 Android build에 통합됩니다. Branch와 commit에 따라 ARM64 vector 복사 대신 C 대체 코드를 사용할 수 있습니다.
- GitHub master에는 단독 Android build 파일이 없으므로 이 fork에서 `scripts/build_android_arm64.sh`를 제공합니다.

시험 결과를 비교할 때에는 실행 파일의 소스 commit, compiler, NDK 또는 AOSP branch, `-W` 사용 여부를 기록해야 합니다.

## 이 문서가 설명하는 것

- 실제 옵션 처리 코드에 있는 모든 공개 옵션과 숨은 옵션
- 기본값과 옵션 조합에 따라 생성되는 Worker
- 메모리 할당, first touch, 1 MiB block 구성
- Block별 mutex queue와 queue 전체 mutex 방식의 차이
- 기본 pattern 15개, 반전·반복 범위 조합, 선택 비율
- Fill·Copy·Check·Invert·File·Network·Disk·CPU·coherency·error Worker
- Modified Adler checksum과 word 단위 상세 비교 과정
- ARM64 NEON 복사, prefetch, cache 관리 명령의 실제 의미
- Virtual address에서 physical address로 변환한 뒤 LPDDR 위치를 결정하는 경계
- Android에서 가능한 측정과 소스만으로 알 수 없는 항목

## 분석 범위 외 항목

- 공개되지 않은 특정 SoC의 DMC address map
- SLC/LLCC inclusive/exclusive 정책
- Physical address bit에서 LPDDR channel·bank·row를 계산하는 실제 공식
- Cache line 교체, 연속 쓰기, prefetch의 정확한 내부 알고리즘
- SoC별 RAS·ECC interrupt와 kernel 로그 연결
- stressapptest 실행 중 발생한 reboot/OOM/thermal shutdown의 원인 판정

이 문서는 메모리 부하 과정과 데이터 검사 방법을 설명합니다. 개별 문제의 원인은 kernel 로그, pstore, watchdog, LMKD, 온도, PMIC, RAS, DMC counter를 함께 확인하여 판정해야 합니다.

<sub><em>Root cause: 관찰된 failure를 직접 발생시킨 최종 원인입니다.</em></sub><br>
<sub><em>RAS: Reliability, Availability, Serviceability의 약어이며 hardware error 검출·기록·복구 기능을 의미합니다.</em></sub>

## 용어상 주의

소스 코드의 명칭과 이 문서에서 사용하는 명칭은 다음과 같습니다.

- `page`: 소스 코드에서는 queue가 관리하는 단위를 의미합니다. 문서에서는 Linux page와 구분하기 위해 기본 1 MiB `SAT block`이라고 합니다.
- `CRC`: 함수 이름에 사용된 표현입니다. 실제 알고리즘은 `modified Adler checksum`입니다.
- Pattern `bus width`: 32-bit pattern word가 반복되는 범위를 의미합니다.
- `physical address`: CPU와 NoC가 사용하는 system physical address를 의미합니다.

문서에서는 `SAT block`, `Linux page`, `checksum`, `system physical address`, `DRAM 내부 위치`를 서로 다른 용어로 사용합니다.

<sub><em>SAT block: stressapptest queue가 관리하는 논리적 메모리 단위이며 기본 크기는 1 MiB입니다.</em></sub><br>
<sub><em>DRAM coordinate: DMC가 선택하는 channel, rank, bank, row 및 column의 조합입니다.</em></sub>

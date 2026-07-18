# 문서 범위와 버전

## 분석 기준

이 문서는 다음 checkout을 직접 분석한 결과입니다.

| 항목 | 값 |
|---|---|
| upstream | `stressapptest/stressapptest` |
| branch | `master` |
| commit | `73b9df227e89cd52b09852056843610722b7b7ae` |
| describe | `v1.0.11-2-g73b9df2` |
| 주요 target | Android/Linux AArch64, mobile LPDDR SoC |
| 기본 SAT block | 1 MiB |
| 코드 cache-line 상수 | 64 B |

문서의 line number와 구현 설명은 이 commit을 기준으로 한다. 이후 upstream이 변경되면 parser, ARM assembly, 기본값을 다시 대조해야 한다.

## GitHub master와 AOSP mirror의 버전 관계

현재 GitHub master의 최신 commit은 Android 전용 파일을 제거했다. Android Open Source Project는 별도의 `platform/external/stressapptest` mirror와 `Android.bp`를 유지한다.

두 tree는 독립된 commit 이력을 가진다. 분석과 실험에는 실행 binary가 생성된 tree와 commit을 적용한다. 현재 확인된 차이는 다음과 같다.

- GitHub master: 현재 ARM64 NEON `AdlerMemcpyAsm()` 구현이 존재한다.
- 별도 AOSP mirror: `Android.bp`를 통한 Android build 통합을 제공하며 branch/commit에 따라 ARM64 vector copy가 fallback C 경로를 사용할 수 있다.
- GitHub master: standalone Android 빌드 파일이 없으므로 이 fork의 `scripts/build_android_arm64.sh`를 제공한다.

실험 결과를 비교할 때는 반드시 binary의 source commit, compiler, NDK/AOSP branch, `-W` 사용 여부를 기록해야 한다.

## 이 문서가 설명하는 것

- argument parser에 존재하는 모든 public option과 숨은 option
- 기본값 및 option 조합에 따른 worker 구성
- memory allocation, first touch, 1 MiB block 구성
- fine-lock queue와 coarse queue의 차이
- 15개 pattern family, inverted/width variant, 선택 가중치
- fill/copy/check/invert/file/network/disk/CPU/coherency/error worker
- modified Adler checksum과 slow compare 경로
- ARM64 NEON copy, prefetch, cache maintenance의 실제 의미
- virtual-to-physical 변환과 LPDDR address decode의 경계
- Android에서 가능한 측정과 소스만으로 알 수 없는 항목

## 분석 범위 외 항목

- 특정 vendor SoC의 confidential DMC address map
- SLC/LLCC inclusive/exclusive 정책
- physical address bit와 LPDDR channel/bank/row의 실제 공식
- cache replacement, write streaming, prefetcher의 정확한 내부 알고리즘
- vendor RAS/ECC interrupt 및 kernel log 연결
- stressapptest 실행 중 발생한 reboot/OOM/thermal shutdown의 원인 판정

이 문서는 부하 생성 메커니즘과 검증 경로를 설명한다. 개별 failure의 root cause는 kernel log, pstore, watchdog, LMKD, thermal, PMIC, RAS 및 DMC counter를 결합하여 판정한다.

<sub><em>Root cause: 관찰된 failure를 직접 발생시킨 최종 원인입니다.</em></sub><br>
<sub><em>RAS: Reliability, Availability, Serviceability의 약어이며 hardware error 검출·기록·복구 기능을 의미합니다.</em></sub>

## 용어상 주의

소스의 역사적 명칭과 이 문서의 표준 명칭은 다음과 같다.

- `page`: 소스에서 queue 관리 단위를 지칭한다. 문서 표준 명칭은 기본 1 MiB `SAT block`이다.
- `CRC`: 함수명에 사용된 명칭이다. 실제 알고리즘의 문서 표준 명칭은 `modified Adler checksum`이다.
- pattern `bus width`: 32-bit pattern word의 반복 폭을 지칭한다.
- `physical address`: CPU와 NoC가 사용하는 system physical address를 지칭한다.

문서에서는 `SAT block`, `Linux page`, `checksum`, `system physical address`, `DRAM coordinate`를 각각 독립된 용어로 사용한다.

<sub><em>SAT block: stressapptest queue가 관리하는 논리적 메모리 단위이며 기본 크기는 1 MiB입니다.</em></sub><br>
<sub><em>DRAM coordinate: DMC가 선택하는 channel, rank, bank, row 및 column의 조합입니다.</em></sub>

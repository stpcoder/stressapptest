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

## GitHub master와 AOSP mirror는 동일하지 않다

현재 GitHub master의 최신 commit은 Android 전용 파일을 제거했다. 반면 Android Open Source Project에는 별도의 `platform/external/stressapptest` mirror와 `Android.bp`가 존재한다.

두 tree는 같은 시점의 코드라고 가정하면 안 된다. 이 문서가 확인한 중요한 차이는 다음과 같다.

- GitHub master: 현재 ARM64 NEON `AdlerMemcpyAsm()` 구현이 존재한다.
- 별도 AOSP mirror: Android build 통합은 쉽지만 branch/commit에 따라 ARM64 vector copy가 fallback C 경로일 수 있다.
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

## 이 문서가 보장하지 않는 것

- 특정 vendor SoC의 confidential DMC address map
- SLC/LLCC inclusive/exclusive 정책
- physical address bit와 LPDDR channel/bank/row의 실제 공식
- cache replacement, write streaming, prefetcher의 정확한 내부 알고리즘
- vendor RAS/ECC interrupt 및 kernel log 연결
- stressapptest 실행 중 발생한 reboot/OOM/thermal shutdown의 원인 판정

즉 이 문서는 “부하 생성 메커니즘”을 설명한다. 개별 failure의 root cause는 kernel log, pstore, watchdog, LMKD, thermal, PMIC, RAS, DMC counter를 결합해 별도로 판정해야 한다.

## 용어상 주의

소스는 다음 이름을 역사적으로 사용한다.

- `page`: 대부분 Linux page가 아니라 기본 1 MiB SAT block을 뜻한다.
- `CRC`: 표준 CRC polynomial이 아니라 modified Adler checksum을 뜻한다.
- pattern `bus width`: 실제 LPDDR DQ width가 아니라 32-bit word 반복 폭이다.
- `physical address`: system PA이며 DRAM row 주소가 아니다.

문서에서는 혼동을 줄이기 위해 가능한 한 `SAT block`, `Linux page`, `checksum`으로 구분한다.

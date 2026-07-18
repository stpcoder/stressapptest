# stressapptest 모바일 ARM64 한글 분석판

이 저장소는 [stressapptest 공식 저장소](https://github.com/stressapptest/stressapptest)를 기반으로, Android/mobile ARM64 환경에서 코드를 읽고 시험을 설계할 수 있도록 한글 분석 매뉴얼과 standalone NDK 빌드 도구를 추가한 개인 fork입니다.

> 기준 소스: upstream commit `73b9df227e89cd52b09852056843610722b7b7ae` (`v1.0.11-2-g73b9df2`)
> 문서 목적: 장애 원인을 미리 단정하는 것이 아니라, stressapptest가 어떤 데이터와 worker로 언제 read/write/verify를 수행하고 그것이 cache·system memory·LPDDR traffic으로 어떻게 이어지는지 설명합니다.

## 가장 먼저 알아야 할 결론

- 기본값에서는 online logical CPU 수만큼 `CopyThread`를 만듭니다. `-m N`으로 바꿀 수 있습니다.
- 테스트 메모리는 기본 1 MiB SAT block으로 나뉩니다. 이는 Linux page나 LPDDR row가 아닙니다.
- worker는 고정 주소 영역을 소유하지 않고 공용 queue에서 random valid/empty block을 얻습니다. 한 block 안에서는 순차적으로 접근합니다.
- 기본 copy는 source read, modified-Adler checksum, destination write를 한 번의 loop에서 수행합니다.
- Android anonymous memory는 일반 cacheable memory입니다. stressapptest는 cache를 끄거나 LPDDR을 직접 접근하지 않습니다.
- 큰 working set, 여러 core의 동시 stream, random 1 MiB block 이동으로 cache miss와 dirty eviction을 반복시켜 system memory traffic을 만듭니다.
- `-W`의 ARM64 `ld1/st1`도 일반 cached access입니다. x86 non-temporal store와 동일하게 해석하면 안 됩니다.
- `CRC`라는 이름을 쓰지만 실제 검증 함수는 네 누산기를 사용하는 modified Adler checksum입니다.
- virtual-to-physical 변환 결과는 system physical address일 뿐, LPDDR channel/bank/row 좌표가 아닙니다.
- SAT 출력 bandwidth는 논리적 copy byte입니다. 실제 LPDDR byte/command는 DMC·NoC·SLC PMU로 확인해야 합니다.

## 한글 GitBook 메뉴

전체 문서는 [`docs/README.md`](docs/README.md)에서 시작합니다.

1. [문서 범위와 버전](docs/00-scope-and-version.md)
2. [프로그램 개요와 전체 구성](docs/01-overview.md)
3. [시작부터 종료까지 실행 순서](docs/02-execution-flow.md)
4. [메모리 할당과 virtual/physical mapping](docs/03-memory-and-physical-mapping.md)
5. [ARM64 cache policy와 실제 LPDDR traffic](docs/04-cache-and-arm64.md)
6. [SAT block, page_entry, valid/empty queue](docs/05-block-and-queue.md)
7. [데이터 pattern 전체 분석](docs/06-patterns.md)
8. [메모리 worker 동작](docs/07-memory-workers.md)
9. [I/O·CPU·coherency worker 동작](docs/08-io-and-system-workers.md)
10. [copy, checksum, 오류 검증 경로](docs/09-copy-and-verification.md)
11. [모든 명령행 옵션](docs/10-all-options.md)
12. [Android ARM64 빌드와 실행](docs/11-android-build-and-run.md)
13. [LPDDR 분석용 시험 recipe](docs/12-test-recipes.md)
14. [PMU·traffic·결과 측정 방법](docs/13-measurement.md)
15. [모바일 환경 한계와 주의사항](docs/14-limitations.md)
16. [소스 코드 지도](docs/15-source-map.md)
17. [초보자를 위한 용어집](docs/16-glossary.md)

GitBook Git Sync용 설정은 [`.gitbook.yaml`](.gitbook.yaml), 목차는 [`docs/SUMMARY.md`](docs/SUMMARY.md)에 있습니다.

## 빠른 빌드

### 일반 Linux

```bash
./configure
make -j"$(nproc)"
./src/stressapptest --help
```

### Android ARM64 standalone NDK

```bash
export ANDROID_NDK_HOME=/path/to/android-ndk
./scripts/build_android_arm64.sh
adb push out/android-arm64/stressapptest /data/local/tmp/
adb shell chmod 755 /data/local/tmp/stressapptest
```

안전한 첫 실행 예시는 메모리 자동 선택을 피하고 명시적으로 크기를 제한합니다.

```bash
adb shell '/data/local/tmp/stressapptest -M 512 -s 60 -m 4 -v 8'
```

## 대표적인 traffic 분리 명령

```bash
# read/check 중심: 테스트 데이터 write worker 없음
stressapptest -M 512 -s 60 -m 0 -c 4

# 기본 mixed read + write + transaction checksum
stressapptest -M 512 -s 60 -m 4

# libc memcpy throughput 중심, copy 중 checksum 생략
stressapptest -M 512 -s 60 -m 4 -F

# read-modify-write와 방향 전환, ARM64 cache maintenance 포함
stressapptest -M 512 -s 60 -m 0 -i 4

# CPU 계산 부하를 같이 인가
stressapptest -M 512 -s 60 -m 4 -C 4
```

> `-d ... --destructive`는 block device를 실제로 덮어쓸 수 있습니다. 개인 데이터가 있는 휴대폰에서는 사용하지 마십시오.

## Upstream과 라이선스

- Upstream: <https://github.com/stressapptest/stressapptest>
- 원본 README: <https://github.com/stressapptest/stressapptest/blob/73b9df227e89cd52b09852056843610722b7b7ae/README.md>
- License: Apache License 2.0. 기존 `COPYING`, `NOTICE`를 유지합니다.

이 문서는 특정 SoC의 confidential address map이나 vendor PMU 정의를 대신하지 않습니다. 실제 channel/bank/row mapping과 DMC counter 의미는 해당 SoC TRM 및 vendor 자료로 최종 확인해야 합니다.

# stressapp 분석

이 저장소는 [stressapptest 공식 저장소](https://github.com/stressapptest/stressapptest)를 Android ARM64 환경에서 분석하고 실행할 수 있도록 정리한 개인 fork입니다. 한글 설명서와 standalone NDK 빌드 도구를 함께 제공합니다.

> 기준 소스: upstream commit `73b9df227e89cd52b09852056843610722b7b7ae` (`v1.0.11-2-g73b9df2`)
> 문서 목적: stressapptest가 메모리를 읽고 쓰는 순서, 오류를 검사하는 방법, cache를 거쳐 LPDDR 부하가 만들어지는 과정을 소스 코드 기준으로 설명합니다.

## 문서 바로 보기

> **GitBook:** [stressapp 분석](https://stressapptest-mobile-arm64.gitbook.io/stressapptest-mobile-arm64-docs/)

GitBook에서는 메뉴, 페이지 목차, 검색, 코드 강조 표시를 사용할 수 있습니다. `master` branch와 연결되어 있어 `docs/`의 변경 내용이 배포 문서에 반영됩니다.

## 먼저 알아둘 내용

- 기본 설정에서는 실행 가능한 논리 CPU 수만큼 `CopyThread`를 만듭니다. `-m N`으로 개수를 바꿀 수 있습니다.
- 테스트 메모리는 기본 1 MiB 크기의 SAT block으로 나뉩니다. worker는 공용 queue에서 읽을 block과 쓸 block을 가져옵니다.
- 한 block 안에서는 주소를 순서대로 읽고 씁니다. 다음 block은 여러 block 가운데 임의로 선택합니다.
- 기본 copy loop는 source read, checksum 계산, destination write를 함께 수행합니다.
- stressapptest는 cache를 끄지 않습니다. 테스트 영역이 cache보다 크고 여러 core가 동시에 작업하면 cache miss와 write-back이 반복되어 LPDDR traffic이 증가합니다.
- `CRC`라는 함수 이름을 사용하지만 실제 검증에는 modified Adler checksum을 사용합니다.
- 프로그램에 표시되는 bandwidth는 worker가 처리한 논리적 byte입니다. 실제 LPDDR traffic은 DMC·NoC·SLC 계측값으로 확인해야 합니다.

<sub><em>Worker: 특정 부하 또는 검증 루프를 수행하는 pthread 실행 단위입니다.</em></sub><br>
<sub><em>Write-back: 수정된 cache line을 하위 cache 또는 system memory 방향으로 기록하는 동작입니다.</em></sub><br>
<sub><em>Physical mapping: virtual address를 system physical address에 대응시키는 변환 관계입니다.</em></sub>

## 문서 목차

처음 읽는다면 아래 순서가 가장 빠릅니다. GitBook에 접근할 수 없는 환경에서는 [`docs/README.md`](docs/README.md)에서 같은 내용을 확인할 수 있습니다.

1. [stressapptest는 어떻게 동작하는가](docs/01-overview.md)
2. [실행 순서 한눈에 보기](docs/02-execution-flow.md)
3. [메모리를 copy하고 오류를 찾는 과정](docs/09-copy-and-verification.md)
4. [cache를 거쳐 LPDDR 부하가 만들어지는 과정](docs/04-cache-and-arm64.md)
5. [메모리 worker별 동작](docs/07-memory-workers.md)
6. [목적별 테스트 명령](docs/12-test-recipes.md)
7. [부하와 오류를 측정하는 방법](docs/13-measurement.md)

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

# CPU 계산 부하를 병행
stressapptest -M 512 -s 60 -m 4 -C 4
```

> `-d ... --destructive`는 block device를 실제로 덮어쓸 수 있습니다. 개인 데이터가 있는 휴대폰에서는 사용하지 마십시오.

## Upstream과 라이선스

- Upstream: <https://github.com/stressapptest/stressapptest>
- 원본 README: <https://github.com/stressapptest/stressapptest/blob/73b9df227e89cd52b09852056843610722b7b7ae/README.md>
- License: Apache License 2.0. 기존 `COPYING`, `NOTICE`를 유지합니다.

특정 SoC의 channel/bank/row mapping과 DMC counter 정의에는 해당 SoC TRM 및 vendor 자료를 적용합니다. 이 문서의 generic 분석 범위는 public stressapptest 소스 구현입니다.

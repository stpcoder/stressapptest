# StressAppTest 설명

이 저장소는 [stressapptest 공식 저장소](https://github.com/stressapptest/stressapptest)를 Android ARM64 환경에서 분석하고 실행할 수 있도록 정리한 개인 fork입니다. 한글 설명서와 standalone NDK 빌드 도구를 함께 제공합니다.

> 기준 소스: upstream commit `73b9df227e89cd52b09852056843610722b7b7ae` (`v1.0.11-2-g73b9df2`)
> 문서 목적: stressapptest가 메모리를 준비하고, Worker를 실행하고, 데이터를 검사하는 전체 과정을 소스 코드 기준으로 설명합니다.

## 문서 바로 보기

> **MkDocs Material:** [StressAppTest 설명](https://stpcoder.github.io/stressapptest/)

문서 사이트는 GitHub Pages에 배포됩니다. `master` branch의 문서가 변경되면 GitHub Actions가 MkDocs Material 사이트를 다시 만들고 자동으로 배포합니다. GitBook과 같은 평평한 흰색 화면, 얇은 구분선, 파란색 현재 메뉴 표시를 사용합니다.

## 핵심 동작

- 기본 설정에서는 온라인 상태의 논리 CPU 수만큼 `CopyThread`를 만듭니다. `-m N`으로 개수를 바꿀 수 있습니다.
- 테스트 메모리는 기본 1 MiB 크기의 SAT block으로 나뉩니다. Worker는 공용 queue에서 읽을 block과 쓸 block을 하나씩 가져옵니다.
- 한 block 안에서는 주소를 순서대로 읽고 씁니다. 다음 block은 여러 block 가운데 임의로 선택합니다.
- 기본 복사 과정은 원본 block 읽기, checksum 계산, 대상 block 쓰기를 함께 수행합니다.
- stressapptest는 cache가 켜진 일반 메모리를 사용합니다. 테스트 영역이 cache보다 크고 여러 core가 동시에 작업하면 cache miss와 write-back이 반복되어 LPDDR 접근량이 증가합니다.
- `CRC`라는 함수 이름을 사용하지만 실제 검증에는 modified Adler checksum을 사용합니다.
- 프로그램에 표시되는 처리량은 Worker가 처리한 논리적 byte입니다. 실제 LPDDR 접근량은 DMC·NoC·SLC 계측값으로 확인해야 합니다.

<sub><em>Worker: 특정 부하 또는 검증 루프를 수행하는 pthread 실행 단위입니다.</em></sub><br>
<sub><em>Write-back: 수정된 cache line을 하위 cache 또는 system memory 방향으로 기록하는 동작입니다.</em></sub><br>
<sub><em>Physical mapping: virtual address를 system physical address에 대응시키는 변환 관계입니다.</em></sub>

## 문서 목차

처음 읽는다면 아래 순서가 가장 빠릅니다. 웹사이트에 접근할 수 없는 환경에서는 [`docs/README.md`](docs/README.md)에서 같은 내용을 확인할 수 있습니다.

1. [stressapptest의 작동 원리](docs/01-overview.md)
2. [실행 순서 한눈에 보기](docs/02-execution-flow.md)
3. [메모리를 복사하고 오류를 찾는 과정](docs/09-copy-and-verification.md)
4. [Cache에서 LPDDR까지 데이터가 이동하는 과정](docs/04-cache-and-arm64.md)
5. [메모리 Worker 종류와 동작](docs/07-memory-workers.md)
6. [목적별 테스트 명령](docs/12-test-recipes.md)
7. [부하와 오류를 측정하는 방법](docs/13-measurement.md)

사이트 메뉴와 화면 설정은 [`mkdocs.yml`](mkdocs.yml), 세부 색상과 본문 스타일은 [`docs/stylesheets/extra.css`](docs/stylesheets/extra.css)에 있습니다.

### 문서 사이트를 로컬에서 확인하기

```bash
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r requirements-docs.txt
mkdocs serve
```

브라우저에서 `http://127.0.0.1:8000/stressapptest/`를 열면 배포 전 화면을 확인할 수 있습니다.

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

## 메모리 접근 방식을 구분하는 실행 명령

```bash
# 읽기와 검사 중심: 테스트 데이터 쓰기 Worker 없음
stressapptest -M 512 -s 60 -m 0 -c 4

# 기본 복사: 읽기 + 쓰기 + checksum 검사
stressapptest -M 512 -s 60 -m 4

# libc memcpy 처리량 중심: 복사 중 checksum 생략
stressapptest -M 512 -s 60 -m 4 -F

# Read-Modify-Write와 접근 방향 전환
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

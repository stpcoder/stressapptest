# stressapp 분석

이 문서는 Android ARM64 기기에서 stressapptest가 메모리에 어떤 부하를 만들고, 데이터 오류를 어떻게 찾는지 설명합니다. 실제 소스 코드를 따라가며 다음 네 가지를 확인합니다.

- 테스트할 메모리를 어떻게 준비하고 block으로 나누는가
- 각 worker가 언제 메모리를 읽고 쓰는가
- cache를 끄지 않은 상태에서 LPDDR traffic이 어떻게 증가하는가
- 읽은 데이터가 맞는지 언제, 어떤 방법으로 검사하는가

<sub><em>Object: 프로그램의 상태와 동작을 함께 보유하는 C++ class instance입니다.</em></sub><br>
<sub><em>Queue: worker가 사용할 SAT block의 상태를 관리하고 동시 접근을 제어하는 자료구조입니다.</em></sub><br>
<sub><em>Checksum: 데이터에서 계산한 요약값을 기대값과 비교하여 변형 여부를 검사하는 값입니다.</em></sub>

## 가장 먼저 알아둘 내용

- 기본 설정에서는 실행 가능한 논리 CPU 수만큼 `CopyThread`를 만듭니다.
- 테스트 메모리는 기본 1 MiB SAT block으로 나뉩니다.
- worker는 공용 queue에서 block을 가져오며, 한 block 안의 주소는 순서대로 처리합니다.
- 기본 copy loop는 source read, checksum 계산, destination write를 함께 수행합니다.
- stressapptest는 cache를 끄지 않습니다. 큰 메모리 영역을 여러 core가 반복해서 처리하여 cache miss와 write-back을 늘립니다.
- destination에 쓴 데이터는 보통 그 block이 나중에 source로 선택될 때 검사됩니다.

## 빠르게 읽는 순서

처음 읽을 때는 다음 다섯 장을 먼저 보면 전체 흐름을 이해할 수 있습니다.

1. [stressapptest는 어떻게 동작하는가](01-overview.md)
2. [실행 순서 한눈에 보기](02-execution-flow.md)
3. [메모리를 copy하고 오류를 찾는 과정](09-copy-and-verification.md)
4. [cache를 거쳐 LPDDR 부하가 만들어지는 과정](04-cache-and-arm64.md)
5. [목적별 테스트 명령](12-test-recipes.md)

세부 구현을 찾을 때는 [소스 코드 찾아보기](15-source-map.md), 어려운 용어는 [용어 설명](16-glossary.md)을 사용합니다.

## 전체 동작

```text
메모리 할당
  → 1 MiB block으로 분할
  → pattern 기록
  → worker 실행
  → source 읽기와 checksum 계산
  → destination 쓰기
  → block을 queue에 반환
  → 다음 선택 때 다시 읽어 오류 검사
```

worker가 처리하는 메모리 영역이 cache보다 크면 cache miss가 늘어납니다. 수정된 cache line이 밀려날 때 write-back이 발생하고, 이 과정이 반복되면서 LPDDR read/write traffic이 만들어집니다.

## 문서에서 구분하는 메모리 단위

- `SAT block`: stressapptest가 관리하는 논리적 block. 기본 1 MiB.
- `Linux page`: MMU translation 단위. 기기 설정에 따라 4/16/64 KiB 등이 가능.
- `cache line`: CPU cache coherency 및 fill/write-back 단위. 코드 기본 상수는 64 B.
- `physical address`: CPU/NoC가 보는 system physical address.
- `DRAM 좌표`: DMC가 해석한 channel/rank/bank group/bank/row/column.
- 코드 위치는 현재 기준 commit의 `파일:줄`로 적습니다.
- “일반적으로”라고 적은 microarchitecture 동작은 ARM architecture가 허용하는 대표 동작이며, 특정 SoC 구현을 보장하지 않습니다.

## 코드 스니펫 읽는 방법

각 장의 핵심 설명에는 실제 repository에서 발췌한 코드가 포함됩니다. 코드 블록 바로 위에는 다음 정보를 표시합니다.

| 표기 | 의미 |
|---|---|
| **파일** | 구현이 존재하는 repository 상대 경로 |
| **함수/구간** | 분석을 시작할 symbol 또는 상수 정의 구간 |
| **기준** | [upstream commit `73b9df2`](https://github.com/stressapptest/stressapptest/tree/73b9df227e89cd52b09852056843610722b7b7ae) |

코드 블록에는 설명에 필요한 부분만 넣었습니다. `...`는 중간 코드가 생략되었다는 뜻입니다. 코드 아래의 **해석**에서 해당 코드가 실제로 하는 일을 설명합니다.

> **소스 기준 예시**
> **파일:** `src/main.cc` · **함수:** `main()` · **기준:** `73b9df2`

```cpp
if (!sat->ParseArgs(argc, argv)) {
  sat->bad_status();
} else if (!sat->Initialize()) {
  sat->bad_status();
} else if (!sat->Run()) {
  sat->bad_status();
}
sat->PrintResults();
```

**해석:** command line 해석, 자원 초기화, 실제 부하 실행이 순차적으로 호출됩니다. 어느 단계에서 실패하더라도 결과 출력과 정리 단계가 이어집니다.

주요 기술 용어는 각 장의 최초 사용 위치에 다음 형식으로 정의합니다.

<sub><em>용어: 해당 장에서 적용하는 기술적 의미와 범위입니다.</em></sub>

## 안전 경고

stressapptest는 정상적으로 동작해도 Android foreground/service, LMKD, thermal governor, DVFS, UFS와 다른 subsystem에 큰 영향을 줄 수 있습니다. 처음에는 `-M`과 `-s`를 작게 명시하고, block device 대상 `-d --destructive`는 사용하지 마십시오.

# stressapptest 모바일 ARM64 한글 매뉴얼

이 문서는 stressapptest의 object, queue, worker, pattern, checksum 및 ARM64 instruction을 소스 코드의 실행 순서에 따라 설명합니다. 분석 범위는 다음 항목으로 구성됩니다.

- 메모리는 언제 할당되고 언제 실제 physical page가 생기는가?
- 1 MiB block과 Linux page, cache line, LPDDR row는 어떻게 다른가?
- worker는 언제 source를 읽고 destination을 쓰는가?
- 어떤 시점에 오류를 발견하며 destination write는 즉시 검증되는가?
- cache를 끄지 않고도 왜 LPDDR traffic이 증가하는가?
- `-m`, `-c`, `-i`, `-C`, `-W`, `-F`는 traffic을 어떻게 바꾸는가?
- virtual address, physical address, DRAM channel/bank/row mapping은 무엇이 다른가?
- SAT bandwidth와 실제 DMC bandwidth가 왜 다를 수 있는가?

<sub><em>Object: 프로그램의 상태와 동작을 함께 보유하는 C++ class instance입니다.</em></sub><br>
<sub><em>Queue: worker가 사용할 SAT block의 상태를 관리하고 동시 접근을 제어하는 자료구조입니다.</em></sub><br>
<sub><em>Checksum: 데이터에서 계산한 요약값을 기대값과 비교하여 변형 여부를 검사하는 값입니다.</em></sub>

## 권장 읽기 순서

기본 분석에는 다음 읽기 순서를 권장합니다.

1. [프로그램 개요와 전체 구성](01-overview.md)
2. [시작부터 종료까지 실행 순서](02-execution-flow.md)
3. [SAT block과 queue](05-block-and-queue.md)
4. [메모리 worker](07-memory-workers.md)
5. [copy와 검증](09-copy-and-verification.md)
6. [cache와 ARM64](04-cache-and-arm64.md)
7. [모든 옵션](10-all-options.md)

LPDDR 개발·분석 업무에는 [시험 recipe](12-test-recipes.md)와 [측정 방법](13-measurement.md)을 추가로 적용합니다.

## 동작 요약

> stressapptest는 anonymous virtual memory를 기본 1 MiB SAT block으로 분할하고, 여러 worker가 pseudo-random으로 선택한 block에 순차 read·checksum·write를 수행한다. working set이 cache capacity를 초과하면 refill과 dirty eviction이 반복적으로 발생한다.

## 문서 표기 원칙

- `SAT block`: stressapptest가 관리하는 논리적 block. 기본 1 MiB.
- `Linux page`: MMU translation 단위. 기기 설정에 따라 4/16/64 KiB 등이 가능.
- `cache line`: CPU cache coherency 및 fill/write-back 단위. 코드 기본 상수는 64 B.
- `physical address`: CPU/NoC가 보는 system physical address.
- `DRAM 좌표`: DMC가 해석한 channel/rank/bank group/bank/row/column.
- 코드 위치는 현재 기준 commit의 `파일:줄`로 적습니다.
- “일반적으로”라고 적은 microarchitecture 동작은 ARM architecture가 허용하는 대표 동작이며, 특정 SoC 구현을 보장하지 않습니다.

## 소스 코드 스니펫 읽는 방법

각 장의 핵심 설명에는 실제 repository에서 발췌한 코드가 포함됩니다. 코드 블록 바로 위에는 다음 정보를 표시합니다.

| 표기 | 의미 |
|---|---|
| **파일** | 구현이 존재하는 repository 상대 경로 |
| **함수/구간** | 분석을 시작할 symbol 또는 상수 정의 구간 |
| **기준** | [upstream commit `73b9df2`](https://github.com/stressapptest/stressapptest/tree/73b9df227e89cd52b09852056843610722b7b7ae) |

코드 블록은 동작을 설명하기 위해 필요한 연속 구간만 발췌합니다. `...`가 있는 경우 중간 구현이 생략되었음을 의미합니다. 코드 아래의 **해석**은 해당 스니펫에서 직접 확인되는 사실과 모바일 SoC 관점의 의미를 구분하여 기술합니다.

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

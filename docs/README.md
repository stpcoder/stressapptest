# stressapptest 모바일 ARM64 한글 매뉴얼

이 문서는 stressapptest를 단순히 “메모리를 많이 사용하는 프로그램”으로 설명하지 않습니다. 현재 소스의 object, queue, worker, pattern, checksum, ARM64 instruction을 연결하여 다음 질문에 답합니다.

- 메모리는 언제 할당되고 언제 실제 physical page가 생기는가?
- 1 MiB block과 Linux page, cache line, LPDDR row는 어떻게 다른가?
- worker는 언제 source를 읽고 destination을 쓰는가?
- 어떤 시점에 오류를 발견하며 destination write는 즉시 검증되는가?
- cache를 끄지 않고도 왜 LPDDR traffic이 증가하는가?
- `-m`, `-c`, `-i`, `-C`, `-W`, `-F`는 traffic을 어떻게 바꾸는가?
- virtual address, physical address, DRAM channel/bank/row mapping은 무엇이 다른가?
- SAT bandwidth와 실제 DMC bandwidth가 왜 다를 수 있는가?

## 권장 읽기 순서

처음 보는 독자는 다음 순서가 좋습니다.

1. [프로그램 개요와 전체 구성](01-overview.md)
2. [시작부터 종료까지 실행 순서](02-execution-flow.md)
3. [SAT block과 queue](05-block-and-queue.md)
4. [메모리 worker](07-memory-workers.md)
5. [copy와 검증](09-copy-and-verification.md)
6. [cache와 ARM64](04-cache-and-arm64.md)
7. [모든 옵션](10-all-options.md)

LPDDR 개발·분석 엔지니어라면 이후 [시험 recipe](12-test-recipes.md)와 [측정 방법](13-measurement.md)을 함께 보십시오.

## 한 문장으로 표현한 동작

> 큰 anonymous virtual memory를 1 MiB 단위로 관리하고, 여러 worker가 random block을 선택한 뒤 block 내부를 순차적으로 read/checksum/write하여 cache capacity를 넘는 다중 stream과 dirty eviction을 지속적으로 만든다.

## 문서 표기 원칙

- `SAT block`: stressapptest가 관리하는 논리적 block. 기본 1 MiB.
- `Linux page`: MMU translation 단위. 기기 설정에 따라 4/16/64 KiB 등이 가능.
- `cache line`: CPU cache coherency 및 fill/write-back 단위. 코드 기본 상수는 64 B.
- `physical address`: CPU/NoC가 보는 system physical address.
- `DRAM 좌표`: DMC가 해석한 channel/rank/bank group/bank/row/column.
- 코드 위치는 현재 기준 commit의 `파일:줄`로 적습니다.
- “일반적으로”라고 적은 microarchitecture 동작은 ARM architecture가 허용하는 대표 동작이며, 특정 SoC 구현을 보장하지 않습니다.

## 안전 경고

stressapptest는 정상적으로 동작해도 Android foreground/service, LMKD, thermal governor, DVFS, UFS와 다른 subsystem에 큰 영향을 줄 수 있습니다. 처음에는 `-M`과 `-s`를 작게 명시하고, block device 대상 `-d --destructive`는 사용하지 마십시오.

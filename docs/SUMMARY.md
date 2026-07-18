# Summary

## 먼저 읽기

- [stressapp 분석](README.md)
- [stressapptest는 어떻게 동작하는가](01-overview.md)
- [실행 순서 한눈에 보기](02-execution-flow.md)
- [메모리를 copy하고 오류를 찾는 과정](09-copy-and-verification.md)

## 메모리 부하가 만들어지는 과정

- [테스트 메모리는 어떻게 준비되는가](03-memory-and-physical-mapping.md)
- [cache를 거쳐 LPDDR 부하가 만들어지는 과정](04-cache-and-arm64.md)
- [메모리 block과 queue](05-block-and-queue.md)
- [메모리에 쓰는 데이터 pattern](06-patterns.md)

## Worker별 동작

- [메모리 worker별 동작](07-memory-workers.md)
- [I/O·CPU worker별 동작](08-io-and-system-workers.md)

## 실행하고 측정하기

- [Android ARM64 빌드와 실행](11-android-build-and-run.md)
- [목적별 테스트 명령](12-test-recipes.md)
- [명령행 옵션 정리](10-all-options.md)
- [부하와 오류를 측정하는 방법](13-measurement.md)

## 참고

- [분석 기준과 버전](00-scope-and-version.md)
- [모바일 환경에서 알아둘 제한사항](14-limitations.md)
- [소스 코드 찾아보기](15-source-map.md)
- [용어 설명](16-glossary.md)

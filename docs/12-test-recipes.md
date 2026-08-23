# 목적에 따른 테스트 명령

이 장은 읽기·검사, 기본 복사, read-modify-write와 CPU 연산 결합 목적별 실행 명령을 정리합니다. 비교 시험에서는 한 번에 한 조건만 변경합니다.

## 테스트 조건 변경 순서

한 번에 여러 옵션을 바꾸면 결과가 달라진 원인을 구분하기 어렵습니다. 다음 순서로 한 항목씩 변경합니다.

1. 테스트 메모리 크기 고정
2. CPU affinity/cpuset 고정
3. Worker 종류 하나 선택
4. Worker 수를 단계적으로 증가
5. `-W` 또는 `-F` 중 하나만 추가하여 비교
6. 필요하면 CPU/UFS/coherency 부하 결합

각 실행은 시작 온도, CPU governor, 백그라운드 프로그램, 화면 상태, 충전기와 전원 조건을 동일하게 유지합니다.

## 명령별로 실행되는 Worker

> **파일:** `src/worker.cc` · **함수:** `CopyThread::Work()` · **기준:** `73b9df2`

```cpp
if (sat_->warm()) {
  CrcWarmCopyPage(&dst, &src);       // -W
} else if (sat_->strict()) {
  CrcCopyPage(&dst, &src);           // 기본
} else {
  memcpy(dst.addr, src.addr,
         sat_->page_length());       // -F
}
```

**코드 설명:** `-W`는 ARM64 vector 명령과 checksum을 사용하는 복사, 기본 실행은 C 코드의 checksum 복사, `-F`는 C library의 `memcpy()`를 선택합니다. `-F`는 Copy source, Invert 전·후, File·Network source/destination의 strict checksum도 생략합니다. 조건 검사 순서 때문에 `-W -F`를 함께 지정하면 Copy는 `-W` 경로를 사용하며 다른 strict checksum은 생략됩니다. 각 방식을 분리해서 실행해야 CPU 명령 구성과 DMC 접근량의 차이를 비교할 수 있습니다.

아래 명령의 512 MiB와 Worker 수는 예시입니다. 대상 휴대폰의 RAM 크기와 온도 여유에 맞게 조정해야 합니다.

## 0. 초기 메모리 쓰기 확인

```bash
stressapptest -M 512 -s 1 -m 0 -c 0 -v 12
```

예상 작동 단계:

1. 8개의 `FillThread`가 512 MiB 전체에 초기 데이터를 씁니다.
2. 본 시험 구간에는 데이터 처리 Worker가 없으므로 관리 정보만 처리합니다.
3. 마지막 8개의 `CheckThread`가 valid block 전체를 읽고 검사합니다.

`-s`를 짧게 설정해도 초기 데이터 쓰기와 종료 시점의 Valid 검사는 실행됩니다. 초기 쓰기와 종료 검사의 로그 시각을 구분하여 확인합니다.

## 1. 메모리 읽기와 검사 중심

```bash
stressapptest -M 512 -s 60 -m 0 -c 4
```

예상:

- 4개의 `CheckThread`가 valid 1 MiB block을 분산해서 선택합니다.
- Block 내부를 앞에서 뒤로 읽으며 4 KiB마다 checksum을 계산합니다.
- 정상 일치 경로에는 테스트 데이터 write가 없습니다. Mismatch 상세 처리에는 expected 복구 write가 있습니다.
- Queue와 block 상태 정보에는 쓰기가 발생합니다.
- DMC 처리량에서 읽기의 비율이 높아질 수 있습니다.

Worker 수를 늘릴 때는 다음 명령을 사용합니다.

```text
-c 1 → 2 → 4 → 8
```

Worker 수 증가에 따른 읽기 bandwidth, cache refill, DMC 대기열 포화 상태를 확인합니다.

## 2. 기본 읽기·쓰기 복사

```bash
stressapptest -M 512 -s 60 -m 4
```

예상:

- 원본 1 MiB를 앞에서 뒤로 순차 읽기
- 원본 checksum 계산
- 대상 1 MiB에 일반 cacheable 방식으로 순차 쓰기
- 대상의 dirty cache line 교체와 write-back
- 반복할 때마다 원본과 대상 block을 다시 선택

메모리에 부하를 주면서 데이터 정확성을 검사하는 기본 실행 방식입니다.

## 3. `memcpy()` 처리량 비교

```bash
stressapptest -M 512 -s 60 -m 4 -F
```

기본 복사 방식과 다음 항목을 비교합니다.

- stressapptest가 출력한 MB/s
- CPU cycle과 실행 명령 수
- L1/L2/SLC refill
- DMC 읽기·쓰기 byte
- 대상에 쓴 데이터의 오류가 검출되기까지 걸린 시간

`-F`에서 DMC bandwidth가 증가하면 checksum 연산이 줄어든 효과와 bionic `memcpy()`의 읽기·쓰기 효율을 분석합니다. Cache 우회 여부는 명령 trace, cache PMU, DMC counter를 사용하여 별도로 확인해야 합니다.

<sub><em>Bottleneck: 전체 처리율을 제한하는 계산, memory 또는 synchronization 자원입니다.</em></sub>

## 4. ARM64 vector 복사

```bash
stressapptest -M 512 -s 60 -m 4 -W
```

예상:

- 64 B 단위 `ld1` 읽기와 `st1` 쓰기
- `prfm pldl1strm`
- Vector 명령으로 checksum 계산
- 일반 cacheable 대상 메모리

비교 대상:

```text
기본 방식과 `-W`, `-F` 비교
```

세 방식의 CPU 사용률, 완료된 명령 수, DMC bandwidth를 함께 측정하여 checksum 계산, vector 복사, C library `memcpy()`의 영향을 구분합니다.

## 5. Read-Modify-Write와 데이터 반전

```bash
stressapptest -M 512 -s 60 -m 0 -i 4 --invert-range full
```

예상:

- 같은 SAT 작업 단위 전체를 낮은 주소 방향과 높은 주소 방향으로 네 번 read-modify-write
- 작업 전과 후에 checksum 검사
- 64 B마다 ARM64 `dc cvau` 기반 cache 관리 명령 실행
- 여러 barrier와 cache 상태 변경

`InvertThread`의 결과에는 네 번의 read-modify-write, 접근 방향 전환, cache 관리 명령, barrier 실행 시간이 포함됩니다. `CopyThread`와 비교할 때에는 프로그램이 계산한 논리적 처리량과 DMC의 읽기·쓰기 byte를 함께 확인해야 합니다.

## 6. CPU 계산 부하 추가

```bash
stressapptest -M 512 -s 300 -m 4 -C 4
```

예상:

- `CopyThread`의 메모리 읽기·쓰기
- `CpuStressThread`의 부동소수점 연산
- CPU와 메모리의 전력·온도 한계 경쟁
- DVFS 또는 온도 제한에 의한 성능 저하 가능성

이 구성은 SoC 전체 전력과 온도를 높이는 시험에 사용합니다. `-C`를 늘려 CPU 전력 사용량이 커지면 DVFS 또는 온도 제한 때문에 메모리 Worker의 처리량이 감소할 수 있습니다.

<sub><em>Power corner: 여러 hardware block의 동시 동작으로 전력, 전압 또는 온도 조건이 한계에 접근하는 시험 상태입니다.</em></sub>

## 7. Cache 일관성 검사

```bash
stressapptest -M 256 -s 60 -m 0 --cc_test
```

예상:

- 설정한 CPU 수만큼 coherency thread 생성
- 소수의 shared cache line 쓰기 권한을 CPU 사이에서 반복 이동
- Snoop과 쓰기 권한 요청 증가
- DRAM data bandwidth는 낮을 수 있음

다음 옵션으로 공동 cache line 수와 한 번에 값을 증가시키는 횟수를 바꿀 수 있습니다.

```bash
stressapptest -M 256 -s 60 -m 0 \
  --cc_test --cc_line_count 8 --cc_inc_count 10000
```

## 8. 잘못된 주소의 데이터 검출

```bash
stressapptest -M 512 -s 60 -m 4 --tag_mode
```

각 cache line의 첫 word에 가상 주소 tag를 기록합니다. 일반 pattern 시험과 별도 항목으로 실행합니다. 이 항목은 memory Worker 구성으로 단독 실행합니다.

## 9. 주기적인 부하 정지와 재시작

```bash
stressapptest -M 512 -s 300 -m 8 \
  --pause_delay 60 --pause_duration 10
```

약 60초마다 부하를 만드는 Worker를 10초 동안 멈춘 뒤 다시 시작합니다. 재시작 시점의 다음 정보를 관찰합니다.

- CPU 주파수 상승 과정
- DMC 주파수 전환
- PMIC 전류 변화
- 온도 변화
- 메모리 오류 발생 시각

각 정보의 timestamp를 stressapptest 로그와 맞춰야 합니다.

## 10. 파일 I/O와 UFS DMA 부하 추가

Filesystem에 충분한 여유 공간이 있고 저장 장치 수명에 미치는 영향을 허용할 수 있을 때만 사용합니다.

```bash
stressapptest -M 512 -s 120 -m 4 \
  -f /data/local/tmp/sat-a.bin
```

이 방식은 CPU 메모리 복사와 UFS·filesystem·DMA 접근을 동시에 발생시킵니다. LPDDR 중심 시험 결과와 저장 장치 경로가 포함된 결과를 구분해야 합니다.

## 비교할 테스트 조합

| 구분 | 명령 핵심 | 주로 확인할 항목 |
|---|---|---|
| A | `-m 0 -c N` | 읽기와 검사 처리량 변화 |
| B | `-m N` | Checksum을 포함한 읽기·쓰기 |
| C | `-m N -W` | ARM64 vector 복사 |
| D | `-m N -F` | 최적화된 `memcpy()` 처리량 |
| E | `-m 0 -i N` | Read-modify-write와 cache 관리 명령 |
| F | `-m N -C N` | 메모리와 CPU의 동시 전력 부하 |
| G | `-m 0 --cc_test` | CPU 사이 cache line 쓰기 권한 이동 |
| H | `-m N -f file` | 메모리와 저장 장치 DMA 동시 부하 |

각 조건은 동일한 시작 온도와 전원 상태에서 3회 이상 반복합니다.

## 쓰기 전용 부하가 필요한 경우

현재 공개 옵션에는 쓰기만 지속하는 메모리 Worker가 없습니다.

- `FillThread`: 초기화 단계에서 한 번 전체 메모리에 씁니다.
- `CopyThread`: 원본을 읽고 대상에 씁니다.
- `InvertThread`: 값을 읽고 bit를 반전한 뒤 같은 주소에 씁니다.
- `CheckThread`: 테스트 데이터를 읽고 검사합니다.

Stressapptest의 기본 Worker는 데이터를 이동하면서 정확성을 검사합니다. 지속적인 write-only 접근은 별도 Worker 또는 memory bandwidth 도구로 구성합니다.

## 추가 검증 옵션

| 옵션 | 기능 | 로그와 비교 항목 |
|---|---|---|
| `-P <ID\|이름[,ID\|이름...]>` | Pattern을 지정하고 여러 항목을 입력 순서대로 SAT 작업 단위에 순환 배정 | Pattern 이름과 입력 순서 |
| `--fill-threads <N>` | 초기 Pattern을 동시에 기록하는 Fill Worker 수 지정 | 병렬 write 수와 초기 write 부하 |
| `--fill-direction <up\|down>` | SAT 작업 단위 내부의 Fill store 진행 방향 지정 | 주소 진행 방향과 prefetch 조건 |
| `--fill-verify-every <N>` | SAT 작업 단위 번호 기준 N 간격의 작업 단위를 즉시 검사 | `fill/immediate_check` |
| `--fill-yield-bytes <byte>` | 지정 byte 기록마다 실행권 양보 | 연속 write 길이와 Worker interleaving |
| `--verify-after-fill` | 초기 Fill 직후 전체 SAT 작업 단위를 한 번씩 검사 | `post_fill/full_check` |
| `--prefault-pages` | 메인 스레드가 각 SAT 작업 단위를 운영체제 page 크기 간격으로 기록한 후 Fill 시작 | 미할당 물리 페이지의 할당 시점과 순서 |
| `--post-fill-delay <초>` | Fill 종료 후 post-fill 검사 또는 Runtime Worker 시작까지 대기 | 마지막 Fill write 이후 경과 시간 |
| `--runtime-start-delay <초>` | Queue 구성 이후 Runtime Worker 시작까지 대기 | Runtime 시작 전 경과 시간 |
| `--pattern-byte-offset <byte>` | Pattern 시작 위치와 expected checksum 위치를 함께 이동 | Pattern 전환 위치와 cache-line 주소 경계 |
| `--fill-preset <none\|zero\|one>` | 최종 Pattern Fill 전에 전체 영역을 지정값으로 기록 | Bit 전환 방향과 선행 write 이력 |
| `--copy-verify-destination` | Copy 직후 destination SAT 작업 단위를 검사 | `copy/destination_check` |
| `--final-check-threads <N>` | Runtime Check와 종료 검사를 분리하고 종료 Worker 수 지정 | `check/final_check` read 동시성 |
| `--skip-final-check` | Runtime Check 종료 drain과 별도 종료 검사 생략 | Fill·Runtime 단계 로그 분리 |
| `--ddr-freq <값\|목록\|all>` | 한 주파수를 유지하거나 여러 주파수를 순환 | `cur_mode`, `cur_freq`, write·read·reread 주파수 |
| `--ddr-step <초>` | Sweep mode의 주파수 변경 간격 지정 | 기본값 3초 |
| `--ddr-node <경로>` | 대상 시스템의 주파수 제어용 kernel interface 경로 지정 | 제어 요청이 전달되는 파일 경로 |
| `--dram-map lpddr-v1` | 선택형 물리 주소-DRAM 좌표 프로필 적용 | `ch`, `rk`, `sc`, `bg`, `bank`, `row`, `col` |
| 자동 로그 필드 | 검출 Worker, 검사 단계와 Pattern offset 기록 | `worker`, `phase`, `pattern_offset` |

<sub><em>First-touch: 예약된 가상 페이지에 처음 접근하는 동작입니다. 아직 연결된 물리 페이지가 없으면 kernel이 이 시점에 물리 페이지를 할당합니다.</em></sub>

각 옵션이 추가하는 read·write, queue 상태와 검사 범위는 [단계별 오류 검출과 옵션 영향 분석](18-stage-debugging-and-option-risk.md)에서 확인합니다.

## 단계별 진단 명령

Fill, Runtime Check, Copy, Invert와 종료 검사를 분리하는 전체 Android 실행 명령은 [단계별 오류 검출과 옵션 영향 분석](18-stage-debugging-and-option-risk.md)에 정리되어 있습니다.

각 비교에서는 옵션 하나를 변경하고 시작 온도, 전원 상태, CPU affinity와 백그라운드 부하를 동일하게 유지합니다.

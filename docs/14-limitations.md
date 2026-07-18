# 모바일 환경 한계와 주의사항

## Source만으로 알 수 없는 것

stressapptest source가 확정하는 것은 virtual address access 순서다. 다음은 SoC 문서/측정이 필요하다.

- cache replacement victim
- write allocate와 streaming mode 전환
- prefetch depth
- SLC inclusivity/partition
- NoC QoS/reordering
- DMC scheduling/read-write batching
- PA→channel/bank/row hash
- LPDDR command/pin activity

## Userspace test의 경계

stressapptest는 Android application processor의 normal userspace load/store를 사용한다.

- physical address에 직접 load/store하지 않음
- cache를 전역 disable하지 않음
- MMU를 끄지 않음
- DMC register를 program하지 않음
- refresh, timing, training, DVFS를 직접 제어하지 않음

따라서 firmware/bootloader/PHY diag와 coverage가 다르다. 반대로 실제 OS scheduling, page allocation, coherency, I/O DMA, thermal/DVFS가 함께 작동하는 system-level corner를 만들 수 있다.

## 자동 memory 크기의 위험

2 GiB 이상 system에서 기본 target은 total의 95%에서 192 MiB를 뺀 값이다. available memory와 Android service reserve를 충분히 반영하지 않는다.

production phone에서는:

- zram thrashing
- LMKD kill
- system_server/app starvation
- allocation failure
- UI hang

이 발생할 수 있다. `-M`을 명시한다.

## Generic ARM ErrorPoll은 no-op

ErrorPollThread가 존재해도 public generic `OsLayer::ErrorPoll()`은 0을 반환한다. corrected ECC, vendor RAS, secure firmware event는 자동 수집되지 않는다.

`--monitor_mode`와 `--no_errors`를 해석할 때 이 사실을 반영한다.

## ARM64 오류 reread 분류

generic AArch64에서 `has_clflush_`가 false이므로 mismatch 후 `OsLayer::Flush()`가 cache line을 실제 flush하지 않을 수 있다. `actual != reread`에 기반한 read error 분류는 신뢰가 제한된다.

## `dc cvau`는 DRAM flush가 아니다

InvertThread의 ARM64 maintenance는 PoU 방향 clean 및 I-cache invalidate sequence다. raw LPDDR까지 강제 write/read한다고 표현하면 안 된다.

## `-W`는 version-dependent

현재 GitHub master는 ARM64 NEON copy가 있지만 AOSP mirror/older package는 fallback C path일 수 있다. binary가 어느 source에서 빌드됐는지 확인한다.

x86 `-W`의 non-temporal store 설명을 ARM64에 그대로 적용하면 안 된다.

## Physical mapping 제한

- `/proc/self/pagemap` PFN은 privilege 없으면 0으로 가려질 수 있음
- 저장되는 `paddr`는 block 첫 address 기준
- block 안 Linux page는 physical contiguous 보장 없음
- page migration 가능
- `--do_page_map`은 4 KiB/zero-based 가정
- generic channel decoder는 modern mobile LPDDR map이 아님
- `--paddr_base`는 generic build에서 무시

## Pattern width 제한

32/64/128/256 suffix는 logical 32-bit word 반복 폭이다. physical DQ width, burst length, channel width가 아니다.

## Pure write-only 부재

지속적인 memory worker 중 pure write-only mode가 없다. 초기 FillThread만 write-heavy이며 Copy는 mixed, Invert는 RMW다.

## Destination 검증 지연

기본 copy는 source를 checksum하면서 destination을 쓴다. destination을 같은 transaction에서 reread하지 않으므로 destination write corruption은 이후 source 선택 또는 final check에서 발견된다.

## Checksum 한계

modified Adler checksum은 빠른 screening용이며 cryptographic/CRC polynomial guarantee가 없다. Collision 가능성이 이론적으로 존재한다. mismatch 때 slow word compare를 하지만 checksum이 우연히 같으면 slow path가 실행되지 않는다.

## Randomness의 성격

FineLock queue는 고정 seed의 pseudo-random generator를 사용한다. Pattern selection도 일반 `random()` state와 thread scheduling 영향을 받는다. 이는 cryptographic randomness나 uniform physical row randomization을 의미하지 않는다.

DiskThread는 `srandom(time(NULL))`을 호출해 process-global random state에도 영향을 줄 수 있다.

## CPU count와 cpuset mismatch

worker 기본 수는 `_SC_NPROCESSORS_ONLN`, affinity 대상은 `sched_getaffinity` 결과를 사용한다. Android cpuset이 제한되면 worker가 허용 core보다 많을 수 있다.

또한 affinity code는 CPU bit가 연속적이라는 가정을 일부 가지고 있어 sparse CPU mask에서 warning/실패 가능성이 있다.

## CPU frequency test는 x86 전용

`--cpu_freq_test`는 `/dev/cpu/*/msr` 및 x86 CPUID를 사용한다. ARM64 mobile에서는 사용할 수 없다.

## Disk option 구현 주의

- `-d --destructive`는 실제 data 손상 가능
- libaio 없이 direct DiskThread 주요 async path가 동작하지 않음
- device size가 cache 추정값의 3배보다 커야 함
- `--random-threads` initialized setter 누락 의심
- `O_DIRECT` 실패 시 page-cache flush fallback은 system-wide 영향 가능

## Timestamp register

AArch64 `GetTimestamp()`는 `CNTVCT_EL0`를 읽는다. kernel이 EL0 virtual counter access를 허용하지 않는 특수 platform에서는 trap/illegal instruction 가능성을 확인해야 한다.

## Pause parameter 0

`--printsec 0` 또는 `--pause_delay 0`은 내부 next-occurrence 계산의 division에 부적절하다. 양수를 사용한다.

## Built-in 문서와 parser 불일치

- parser: `--reserve_memory`
- help: `--reserve-memory`
- help 누락: `-c`, `--coarse_grain_lock`, `--tag_mode`, `--do_page_map`
- upstream README의 “processor당 두 thread” 설명은 현재 기본 `memory_threads = online CPUs` 코드와 다름

항상 target binary와 동일 commit의 parser를 기준으로 한다.

## 안전한 해석 원칙

```text
SAT data mismatch
  → memory subsystem 경로의 corruption 증거가 될 수 있음
  → 즉시 DRAM cell 불량으로 단정하지 않음

SAT process death
  → root cause가 아님
  → OOM/LMKD/thermal/watchdog/kernel/RAS를 분리

SAT MB/s
  → logical software throughput
  → DMC/LPDDR bandwidth와 동일하지 않음
```

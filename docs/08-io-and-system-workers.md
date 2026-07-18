# I/O·CPU·coherency worker 동작

## FileThread (`-f`)

`-f path` 하나마다 FileThread 하나를 만든다. 기본 `disk_pages_`는 8이므로 기본 block 1 MiB 기준 한 pass에서 8 MiB를 파일로 write한 뒤 8 MiB를 read한다.

### open policy

```text
O_RDWR | O_CREAT | O_SYNC | O_DIRECT
```

`O_DIRECT`가 `EINVAL`로 실패하면 이를 빼고 다시 open한 뒤 filesystem page cache flush 경로를 활성화한다.

### 한 pass

```text
valid SAT block 8개 획득
 → strict이면 source checksum
 → 각 512 B sector에 magic/block/sector/pass tag 삽입
 → 파일 시작부터 순차 write
 → source block을 empty로 반환
 → 파일 시작으로 seek
 → empty SAT block 8개에 순차 read
 → sector tag 검증 및 원 pattern 복원
 → strict이면 block checksum
 → destination을 valid로 반환
```

FileThread는 UFS/storage DMA와 memory/NoC traffic을 함께 만든다. `O_DIRECT`는 Linux page cache를 우회하려는 것이며 CPU L1/L2/SLC bypass가 아니다.

`--filesize`는 한 pass의 파일 크기를 byte로 지정하며 내부적으로 `disk_pages = filesize / SAT block size`로 계산한다. 최소 한 block이다.

## NetworkThread (`-n`, `--listen`)

network port는 TCP 19996이다.

### Listener side

`--listen`은 `0.0.0.0:19996`에 bind/listen하고 connection마다 NetworkSlaveThread를 만든다.

Slave는 512 B aligned local buffer에 1 SAT block을 recv한 뒤 같은 bytes를 sender에게 다시 send한다. 별도 pattern verification은 sender가 수행한다.

### Sender side

`-n ipaddr` 하나마다 NetworkThread 하나를 만든다. 시작 후 15초 기다렸다가 연결한다.

```text
valid source + empty destination 획득
 → strict이면 source checksum
 → source 1 block TCP send
 → reflector가 보낸 1 block recv into destination
 → strict이면 destination checksum
 → destination valid, source empty
```

network worker는 CPU copy, socket buffer, kernel network stack, DMA/NIC/Wi-Fi subsystem을 함께 사용하므로 순수 LPDDR controller test로 분리하기 어렵다.

`--tag_mode`와 함께 사용할 수 없다.

## DiskThread (`-d`)

`-d device-or-file`은 direct device/file random I/O worker 하나를 만든다. FileThread와 달리 SAT 1 MiB file pass가 아니라 sector/block table과 async I/O를 사용한다.

### 기본은 non-destructive

`--destructive`를 주지 않으면 write phase가 disable되고 random location을 read만 한다. 이 경우 기존 disk data는 SAT pattern과 비교하지 않는다.

`--destructive`를 주면 random block에 SAT pattern을 write하고 충분한 queue depth 후 다시 read/verify한다.

> 휴대폰의 실제 block device에 `--destructive`를 사용하면 filesystem, userdata, boot partition을 복구 불가능하게 손상시킬 수 있다.

### 주요 parameter

- sector/alignment: 512 B
- read block 기본: 512 B
- write block: 미지정 시 read block과 같게 보정
- disk cache 기본: worker constructor 기준 16 MiB
- in-flight queue: cache에 들어갈 block 수의 약 150%
- device size 요구: cache size의 3배 초과
- libaio가 build에 있어야 async read/write 경로 사용

### RandomDiskThread

`--random-threads N`은 각 `-d` DiskThread마다 N개의 추가 reader를 만든다. 이들은 shared `DiskBlockTable`에서 initialized block을 random 선택해 검증한다.

현재 기준 코드에는 주의할 부분이 있다. main DiskThread가 block 준비 후 `block->set_initialized()`가 아니라 getter인 `block->initialized()`를 호출한다 (`src/worker.cc:2948`). 따라서 table entry가 initialized로 바뀌지 않아 RandomDiskThread가 기대대로 block을 받지 못할 가능성이 있다. 이 option을 유효하다고 가정하기 전에 target build에서 trace/수정 검증이 필요하다.

## CpuStressThread (`-C`)

`-C N`은 N개의 CPU stress worker를 만든다.

generic ARM/Linux workload는 100개의 double array에 대해 100,000,000회 moving-average 형태의 floating-point 계산을 반복한다 (`src/os.cc:904`).

특성:

- working array가 작아 L1에 머물 가능성이 큼
- 직접적인 DRAM bandwidth generator가 아님
- FP/vector pipeline, CPU dynamic power, thermal, DVFS에 영향
- CopyThread와 함께 사용하면 memory+compute 동시 전력 조건 생성
- 자체 data correctness pass/fail을 평가하지 않음

## CpuCacheCoherencyThread (`--cc_test`)

configured CPU 수만큼 thread를 만들고 각 thread가 CPU에 pin된다.

여러 cache-line-sized structure 중 하나를 pseudo-random 선택하고 자기 thread에 대응하는 byte counter를 반복 증가시킨다. 이후 모든 선택 line의 해당 counter 합이 `cc_inc_count`와 같은지 확인하고 0으로 reset한다.

목적:

- 여러 core 사이 shared cache line ownership 이동
- snoop/invalidate/update
- cache coherency protocol correctness

이 test는 작은 shared data를 집중적으로 ping-pong하므로 primary DRAM bandwidth test가 아니다. cache line이 SLC/DRAM에 자주 내려갈지는 coherency implementation에 의존한다.

관련 option:

- `--cc_line_count`: shared cache-line structure 수, 기본 2
- `--cc_line_size`: auto-detected line size override
- `--cc_inc_count`: 한 batch의 increment 수, 기본 1000

## ErrorPollThread

기본으로 하나 생성되어 `OsLayer::ErrorPoll()`을 약 1초마다 호출한다.

public generic `OsLayer::ErrorPoll()`은 항상 0을 반환한다 (`src/os.cc:739`). 현재 Android ARM generic build는 vendor corrected ECC/RAS event를 자동으로 읽지 않는다.

따라서 mobile에서는 다음을 외부에서 별도 수집해야 한다.

- kernel RAS/EDAC/vendor memory error log
- pstore/ramoops
- watchdog/reboot reason
- DMC/LLCC error register
- secure firmware/SoC-specific diagnostics

`--no_errors`는 이 polling thread를 생성하지 않지만 memory pattern verification을 끄는 option은 아니다.

## CpuFreqThread

`--cpu_freq_test`는 TSC/APERF/MPERF MSR을 읽는 x86 전용 구현이다. AArch64에서는 `CanRun()`이 false를 반환해 초기화가 실패한다.

Android ARM에서 CPU frequency를 확인하려면 이 option 대신 cpufreq sysfs, tracepoint, vendor profiler, simpleperf/Perfetto 등을 사용해야 한다.

## Monitor mode

`--monitor_mode`는 test memory를 할당하지 않고 ErrorPollThread만 실행한다.

하지만 generic ARM ErrorPoll이 no-op이므로 vendor-specific OsLayer 없이 Android에서 실질적인 ECC monitor로 사용하기 어렵다.

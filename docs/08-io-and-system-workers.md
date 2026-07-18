# I/O·CPU·coherency worker 동작

## FileThread (`-f`)

`-f path` 하나마다 FileThread 하나를 만든다. 기본 `disk_pages_`는 8이므로 기본 block 1 MiB 기준 한 pass에서 8 MiB를 파일로 write한 뒤 8 MiB를 read한다.

### open policy

```text
O_RDWR | O_CREAT | O_SYNC | O_DIRECT
```

`O_DIRECT`가 `EINVAL`로 실패하면 이를 빼고 다시 open한 뒤 filesystem page cache flush 경로를 활성화한다.

<sub><em>O_SYNC: write system call의 데이터와 필요한 metadata가 backing storage에 동기화되도록 요청하는 file open flag입니다.</em></sub><br>
<sub><em>O_DIRECT: filesystem page cache 사용을 최소화하도록 kernel에 요청하는 file open flag입니다.</em></sub><br>
<sub><em>Page cache: Linux kernel이 file 데이터를 RAM에 보관하여 file I/O를 처리하는 cache 계층입니다.</em></sub>

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

FileThread는 UFS/storage DMA와 memory/NoC traffic을 동시에 생성한다. `O_DIRECT`의 적용 범위는 Linux page cache이며, DMA coherency와 CPU L1/L2/SLC access는 platform I/O 경로에 따라 처리된다.

<sub><em>DMA: device가 CPU의 개별 load/store 개입 없이 system memory와 데이터를 전송하는 기능입니다.</em></sub>

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

`-d device-or-file`은 direct device/file random I/O worker 하나를 만든다. DiskThread는 sector/block table과 asynchronous I/O를 사용하며, FileThread는 SAT block 단위의 sequential file pass를 사용한다.

<sub><em>Asynchronous I/O: I/O 요청 제출과 완료 수집을 분리하여 여러 요청을 동시에 계류시키는 방식입니다.</em></sub>

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

현재 기준 코드의 main DiskThread는 block 준비 후 상태 조회 함수 `block->initialized()`를 호출한다 (`src/worker.cc:2948`). 상태 설정 함수 `block->set_initialized()` 호출은 해당 경로에 존재하지 않는다. 그 결과 table entry의 initialized flag가 설정되지 않을 수 있으며 RandomDiskThread의 block 획득 조건이 충족되지 않을 수 있다. target build에서 상태 전이 trace를 확인한 후 이 option의 실행 결과를 사용한다.

## CpuStressThread (`-C`)

`-C N`은 N개의 CPU stress worker를 만든다.

generic ARM/Linux workload는 100개의 double array에 대해 100,000,000회 moving-average 형태의 floating-point 계산을 반복한다 (`src/os.cc:904`).

특성:

- working array가 작아 L1에 머물 가능성이 큼
- 주된 workload는 CPU FP/vector calculation
- FP/vector pipeline, CPU dynamic power, thermal, DVFS에 영향
- CopyThread와 함께 사용하면 memory+compute 동시 전력 조건 생성
- worker status는 계산 loop 실행 성공 여부로 기록

## CpuCacheCoherencyThread (`--cc_test`)

configured CPU 수만큼 thread를 만들고 각 thread가 CPU에 pin된다.

여러 cache-line-sized structure 중 하나를 pseudo-random 선택하고 자기 thread에 대응하는 byte counter를 반복 증가시킨다. 이후 모든 선택 line의 해당 counter 합이 `cc_inc_count`와 같은지 확인하고 0으로 reset한다.

목적:

- 여러 core 사이 shared cache line ownership 이동
- snoop/invalidate/update
- cache coherency protocol correctness

이 test는 작은 shared data의 cache-line ownership을 core 사이에서 반복적으로 전환한다. 주된 부하는 snoop, invalidate 및 ownership transaction이며, SLC/DRAM traffic 비율은 coherency implementation에 따라 결정된다.

<sub><em>Cache-line ownership: 특정 core가 cache line을 수정할 수 있도록 coherency protocol이 부여한 권한 상태입니다.</em></sub><br>
<sub><em>Snoop: 다른 cache가 해당 address의 data 또는 ownership을 보유하는지 조회하는 coherency transaction입니다.</em></sub>

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

`--no_errors`는 ErrorPollThread 생성을 비활성화한다. CopyThread, CheckThread 및 final check의 pattern verification 설정은 그대로 유지된다.

## CpuFreqThread

`--cpu_freq_test`는 TSC/APERF/MPERF MSR을 읽는 x86 전용 구현이다. AArch64에서는 `CanRun()`이 false를 반환해 초기화가 실패한다.

Android ARM의 CPU frequency는 cpufreq sysfs, tracepoint, vendor profiler, simpleperf 또는 Perfetto로 측정한다. `--cpu_freq_test`는 AArch64 초기화 단계에서 지원되지 않는 설정으로 처리된다.

## Monitor mode

`--monitor_mode`는 test memory를 할당하지 않고 ErrorPollThread만 실행한다.

generic ARM ErrorPoll은 항상 0을 반환한다. Android에서 ECC event monitor로 사용하려면 vendor-specific `OsLayer::ErrorPoll()` 구현을 연결한다.

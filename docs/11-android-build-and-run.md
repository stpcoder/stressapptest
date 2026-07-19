# Android ARM64 빌드와 실행

이 장에서는 현재 저장소의 소스 코드를 Android ARM64 실행 파일로 빌드하고 휴대폰의 Android shell에서 실행하는 방법을 설명합니다.

## 이 저장소의 Android 빌드 방식

현재 GitHub master에는 단독으로 사용할 수 있는 `Android.bp`가 없습니다. 이 fork는 Android NDK로 소스 코드를 직접 cross-compile하는 `scripts/build_android_arm64.sh`를 제공합니다.

AOSP source tree에 포함하여 빌드할 때에는 해당 branch의 `platform/external/stressapptest/Android.bp`를 사용해야 합니다. 실행 파일에 포함되는 ARM64 복사 코드는 AOSP mirror와 GitHub 저장소의 `src/adler32memcpy.cc`가 다를 수 있으므로 실제 build의 commit을 확인해야 합니다.

## NDK로 단독 실행 binary 빌드하기

필요 조건:

- Android NDK
- macOS 또는 Linux x86_64 build 컴퓨터
- AArch64 휴대폰

```bash
export ANDROID_NDK_HOME=/path/to/android-ndk
export ANDROID_API=30
./scripts/build_android_arm64.sh
```

API level과 출력 경로는 다음과 같이 직접 지정할 수 있습니다.

```bash
./scripts/build_android_arm64.sh 30 /tmp/stressapptest-arm64
```

기본 출력 위치는 다음과 같습니다.

```text
out/android-arm64/stressapptest
```

Build script는 다음 macro를 사용합니다.

```text
HAVE_CONFIG_H
ANDROID
NDEBUG
CHECKOPTS
STRESSAPPTEST_CPU_AARCH64
```

`src/stressapptest_config_android.h`를 출력 경로에 `stressapptest_config.h`라는 이름으로 준비하여 공개 소스의 autoconf include 방식을 맞춥니다.

> **파일:** `scripts/build_android_arm64.sh` · **구간:** AArch64 compiler 호출 · **기준:** 이 fork

```bash
"${cxx}" \
  -std=gnu++11 \
  -O2 \
  -g \
  -fno-omit-frame-pointer \
  -fPIE \
  -pie \
  -pthread \
  -DHAVE_CONFIG_H \
  -DANDROID \
  -DNDEBUG \
  -UDEBUG \
  -DCHECKOPTS \
  -DSTRESSAPPTEST_CPU_AARCH64 \
  -I"${output_dir}" \
  -I"${repo_root}/src" \
  "${source_paths[@]}" \
  -o "${output_dir}/stressapptest"
```

**코드 설명:** NDK의 `aarch64-linux-android<API>-clang++`로 공개 소스 파일을 하나의 PIE 실행 파일로 연결합니다. `STRESSAPPTEST_CPU_AARCH64`는 ARM64용 timestamp, cache 관리 명령, NEON 복사 코드를 선택합니다.

<sub><em>PIE: Position-Independent Executable의 약어이며 ASLR 적용을 위해 고정 virtual address에 의존하지 않도록 생성한 실행 파일입니다.</em></sub><br>
<sub><em>Conditional compilation: compile-time macro 값에 따라 특정 architecture 또는 platform 구현만 binary에 포함하는 방식입니다.</em></sub>

## 빌드 결과 확인

```bash
file out/android-arm64/stressapptest
```

예상 형태:

```text
ELF 64-bit LSB pie executable, ARM aarch64, ... Android ...
```

NDK의 `llvm-readelf`로 CPU architecture와 동적 library 의존성을 확인할 수 있습니다.

```bash
"$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-readelf" \
  -h -d out/android-arm64/stressapptest
```

Linux에서 빌드할 때에는 toolchain 경로의 `darwin-x86_64`를 `linux-x86_64`로 바꿉니다.

## 휴대폰으로 복사

```bash
adb push out/android-arm64/stressapptest /data/local/tmp/stressapptest
adb shell chmod 755 /data/local/tmp/stressapptest
adb shell /data/local/tmp/stressapptest --help
```

`/data/local/tmp`는 Android shell에서 실행 파일을 두는 일반적인 위치입니다. 양산 user build에서는 SELinux, 실행 권한, 성능 counter 권한, pagemap 접근이 제한될 수 있습니다.

## 첫 실행 명령

처음 실행할 때에는 메모리 크기를 직접 지정하고 시험 시간을 짧게 설정합니다.

```bash
adb shell '/data/local/tmp/stressapptest -M 256 -s 30 -m 2 -v 8'
```

초기 결과와 시스템 상태를 확인한 뒤 다음 순서로 부하를 늘립니다.

```text
256 MiB / Worker 2개 / 30초
512 MiB / Worker 4개 / 60초
1 GiB   / 선택한 Worker 전체 / 5분
온도·LMKD·DMC counter를 확인하며 실행 시간 확대
```

## 사용 가능한 CPU 확인

```bash
adb shell 'cat /sys/devices/system/cpu/online'
adb shell 'grep Cpus_allowed_list /proc/self/status'
adb shell 'taskset -p $$'
```

실제 stressapptest 프로세스에 적용된 CPU mask는 실행 중인 PID로 확인합니다.

```bash
adb shell 'pidof stressapptest'
adb shell 'grep Cpus_allowed_list /proc/$(pidof stressapptest)/status'
```

Android shell에 허용된 cpuset이 좁으면 online CPU 수만큼 Worker를 만들어도 일부 CPU에서 여러 Worker가 번갈아 실행됩니다. 재현성을 높이려면 `taskset` 또는 cgroup 설정과 `-m` 값을 함께 기록해야 합니다.

## Big core와 LITTLE core를 구분하여 실행

CPU 번호와 성능 등급은 SoC마다 다르므로 먼저 각 CPU의 capacity를 확인합니다.

```bash
adb shell 'for c in /sys/devices/system/cpu/cpu[0-9]*; do \
  echo -n "$c "; cat "$c/cpu_capacity" 2>/dev/null; done'
```

CPU 4~7만 허용하는 16진수 mask는 `f0`입니다. 실제 mask는 대상 SoC의 CPU 번호를 기준으로 계산해야 합니다.

<sub><em>CPU mask: thread 또는 process가 실행될 수 있는 logical CPU를 bit 단위로 표시한 값입니다.</em></sub><br>
<sub><em>cpuset: Android/Linux cgroup이 process에 허용하는 CPU 집합을 관리하는 기능입니다.</em></sub>

```bash
adb shell 'taskset f0 /data/local/tmp/stressapptest -M 512 -s 60 -m 4'
```

stressapptest가 설정하는 CPU affinity와 외부 `taskset`이 함께 적용됩니다. 로그 상세도를 높여 각 Worker의 CPU mask를 확인해야 합니다. 필요하면 `--no_affinity`를 사용하여 외부 mask 안에서 Android scheduler가 Worker를 배치하게 합니다.

## 실행 중 수집할 정보

별도 terminal에서 다음 정보를 수집합니다.

```bash
adb logcat -b all -v threadtime
adb shell dmesg -w
adb shell 'while true; do cat /proc/meminfo | head -20; sleep 5; done'
```

User build에서는 `dmesg` 접근이 제한될 수 있습니다. 가능한 경우 다음 정보도 수집합니다.

- pstore/ramoops
- `/sys/class/thermal/thermal_zone*/temp`
- CPU/GPU/DMC devfreq
- LMKD log
- vendor RAS/ECC log
- simpleperf/Perfetto
- SLC/LLCC/NoC/DMC PMU

## 종료

```bash
adb shell 'pkill -INT stressapptest'
```

SIGINT 또는 SIGTERM을 보내면 주 thread가 Worker를 정리하고 마지막 전체 검사를 수행합니다. `kill -9`, LMKD의 SIGKILL, kernel panic으로 종료되면 마지막 검사와 정상 종료 통계가 남지 않습니다.

## Android에서 주의해야 할 실행 방법

```bash
# 전체 RAM에서 자동으로 큰 영역을 선택하므로 양산 휴대폰에서 위험
stressapptest -s 3600

# 실제 block device 파괴 위험
stressapptest -d /dev/block/by-name/userdata --destructive

# 공통 ARM 구현에서 지원하지 않아 초기화 실패
stressapptest --cpu_freq_test --cpu_freq_threshold 1000

# 공통 OsLayer가 paddr_base를 무시하고 일반 메모리 할당 수행
stressapptest --paddr_base 0x80000000
```

# Android ARM64 빌드와 실행

이 장에서는 현재 repository를 Android ARM64용 binary로 빌드하고, 휴대폰의 Android console에서 안전하게 시작하는 방법을 설명합니다.

## 이 repository의 Android 빌드 방식

현재 GitHub master에는 standalone `Android.bp`가 없다. 이 fork는 Android NDK로 현재 GitHub source를 직접 cross-compile하는 `scripts/build_android_arm64.sh`를 제공한다.

AOSP source tree 통합 빌드에는 해당 branch의 `platform/external/stressapptest/Android.bp`를 사용한다. 실행 binary의 ARM64 copy 구현은 AOSP mirror commit과 GitHub master commit의 `src/adler32memcpy.cc`를 대조하여 확인한다.

## NDK로 standalone binary 빌드하기

필요 조건:

- Android NDK
- macOS 또는 Linux x86_64 build host
- AArch64 target phone

```bash
export ANDROID_NDK_HOME=/path/to/android-ndk
export ANDROID_API=30
./scripts/build_android_arm64.sh
```

API level과 output directory를 위치 인자로 덮어쓸 수도 있다.

```bash
./scripts/build_android_arm64.sh 30 /tmp/stressapptest-arm64
```

기본 output:

```text
out/android-arm64/stressapptest
```

script는 다음 주요 define을 사용한다.

```text
HAVE_CONFIG_H
ANDROID
NDEBUG
CHECKOPTS
STRESSAPPTEST_CPU_AARCH64
```

`src/stressapptest_config_android.h`를 build output에 `stressapptest_config.h`로 준비해 public source의 autoconf include 방식을 만족시킨다.

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

**해석:** NDK의 `aarch64-linux-android<API>-clang++`로 모든 public source를 하나의 PIE executable로 link합니다. `STRESSAPPTEST_CPU_AARCH64`가 ARM64 timestamp, cache maintenance 및 NEON copy 조건부 구현을 선택합니다.

<sub><em>PIE: Position-Independent Executable의 약어이며 ASLR 적용을 위해 고정 virtual address에 의존하지 않도록 생성한 실행 파일입니다.</em></sub><br>
<sub><em>Conditional compilation: compile-time macro 값에 따라 특정 architecture 또는 platform 구현만 binary에 포함하는 방식입니다.</em></sub>

## 빌드한 binary 확인하기

```bash
file out/android-arm64/stressapptest
```

예상 형태:

```text
ELF 64-bit LSB pie executable, ARM aarch64, ... Android ...
```

NDK의 llvm-readelf로 architecture와 dynamic dependency를 확인할 수도 있다.

```bash
"$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-readelf" \
  -h -d out/android-arm64/stressapptest
```

Linux host에서는 toolchain directory를 `linux-x86_64`로 바꾼다.

## 휴대폰으로 복사하기

```bash
adb push out/android-arm64/stressapptest /data/local/tmp/stressapptest
adb shell chmod 755 /data/local/tmp/stressapptest
adb shell /data/local/tmp/stressapptest --help
```

`/data/local/tmp`는 일반적인 shell 실행 위치다. production user build에서는 SELinux, exec restriction, perf counter permission, pagemap 접근이 제한될 수 있다.

## 처음 실행할 때 사용할 명령

첫 실행에는 명시적 memory 크기와 짧은 runtime을 적용한다.

```bash
adb shell '/data/local/tmp/stressapptest -M 256 -s 30 -m 2 -v 8'
```

초기 결과와 system 상태를 확인한 후 다음 순서로 부하를 확대한다.

```text
256 MiB / 2 workers / 30 s
512 MiB / 4 workers / 60 s
1 GiB   / all selected workers / 5 min
thermal·LMKD·DMC counter를 보며 장시간 확대
```

## 사용할 수 있는 CPU 확인하기

```bash
adb shell 'cat /sys/devices/system/cpu/online'
adb shell 'grep Cpus_allowed_list /proc/self/status'
adb shell 'taskset -p $$'
```

실제 stressapptest process의 mask를 보려면 실행 중 PID에 대해 확인한다.

```bash
adb shell 'pidof stressapptest'
adb shell 'grep Cpus_allowed_list /proc/$(pidof stressapptest)/status'
```

Android shell cpuset이 좁으면 online CPU 수만큼 worker를 만들더라도 허용 core들에서 time-slice할 수 있다. 재현성을 높이려면 `taskset` 또는 적절한 cgroup 설정과 `-m`을 함께 기록한다.

## big core와 LITTLE core를 나눠서 실행하기

CPU numbering은 SoC마다 다르므로 capacity를 먼저 확인한다.

```bash
adb shell 'for c in /sys/devices/system/cpu/cpu[0-9]*; do \
  echo -n "$c "; cat "$c/cpu_capacity" 2>/dev/null; done'
```

CPU 4~7을 허용하는 경우의 hexadecimal mask는 `f0`이다. 실제 실행 mask는 target topology의 CPU numbering을 기준으로 계산한다.

<sub><em>CPU mask: thread 또는 process가 실행될 수 있는 logical CPU를 bit 단위로 표시한 값입니다.</em></sub><br>
<sub><em>cpuset: Android/Linux cgroup이 process에 허용하는 CPU 집합을 관리하는 기능입니다.</em></sub>

```bash
adb shell 'taskset f0 /data/local/tmp/stressapptest -M 512 -s 60 -m 4'
```

stressapptest 자체 affinity와 외부 `taskset`이 상호작용하므로 log verbosity를 높여 worker mask를 확인한다. 필요하면 `--no_affinity`를 사용해 외부 mask 안에서 scheduler가 배치하도록 한다.

## 실행 중 기록할 항목

별도 terminal에서:

```bash
adb logcat -b all -v threadtime
adb shell dmesg -w
adb shell 'while true; do cat /proc/meminfo | head -20; sleep 5; done'
```

user build에서는 `dmesg`가 막힐 수 있다. 가능한 경우 추가 수집:

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

SIGINT/SIGTERM은 main thread가 받아 worker를 정리하고 final check를 수행한다. `kill -9`, LMKD SIGKILL, kernel panic에서는 final check와 정상 통계가 없다.

## Android에서 바로 사용하면 위험한 옵션

```bash
# 전체 RAM 자동 선택: production phone에서 위험
stressapptest -s 3600

# 실제 block device 파괴 위험
stressapptest -d /dev/block/by-name/userdata --destructive

# generic ARM 초기화에서 지원 불가 상태 반환
stressapptest --cpu_freq_test --cpu_freq_threshold 1000

# generic OsLayer가 paddr_base를 무시하고 일반 allocation 수행
stressapptest --paddr_base 0x80000000
```

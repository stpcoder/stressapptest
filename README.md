# StressAppTest Android ARM64 확장판

이 저장소는 공개 [stressapptest](https://github.com/stressapptest/stressapptest)에 Android AArch64 실장기 테스트 기능을 추가한 개인 fork입니다. 이 README는 이 fork에서 추가한 기능과 실행 방법만 설명합니다.

[![Android ARM64 최신 버전 다운로드](https://img.shields.io/badge/Android_ARM64-최신_버전_다운로드-1976d2?style=for-the-badge&logo=android&logoColor=white)](https://github.com/stpcoder/stressapptest/releases/latest/download/stressapptest-android-arm64)

압축 파일과 SHA-256 checksum이 필요하면 [최신 Release 페이지](https://github.com/stpcoder/stressapptest/releases/latest)에서 다운로드할 수 있습니다.

## 추가된 기능

| 기능 | 옵션 | 설명 |
|---|---|---|
| 패턴 선택 | `-P <ID\|이름[,ID\|이름...]>` | 하나 또는 여러 pattern을 지정한 순서대로 block에 순환 배정 |
| DDR 주파수 고정 | `--ddr-freq <주파수>` | 하나의 Qualcomm DDR 주파수를 시험 종료까지 유지 |
| 전체 주파수 sweep | `--ddr-freq all` | 등록된 전체 주파수를 순서대로 반복 |
| 선택 주파수 sweep | `--ddr-freq <목록>` | 쉼표로 지정한 주파수만 입력 순서대로 반복 |
| 전환 간격 | `--ddr-step <초>` | sweep 주파수 유지 시간. 기본값은 3초 |
| AOSS node 변경 | `--ddr-node <경로>` | 기본 debugfs node가 다른 제품에서 사용 |
| 오류 주파수 기록 | 자동 | expected data의 마지막 write, 최초 mismatch read, cache flush 후 reread 시점의 값을 각각 출력 |

기본 Qualcomm AOSS node는 다음 경로입니다.

```text
/sys/kernel/debug/aoss_send_message
```

프로그램은 shell의 `echo`를 실행하지 않고 다음 메시지를 debugfs node에 직접 씁니다.

```text
{class:ddr, res:fixed, val:3196}
```

테스트가 끝난 후 마지막 fixed DDR 요청을 별도로 해제하지 않습니다.

## 휴대폰에 설치

GitHub Release에서 바이너리를 받은 다음 Android 기기에 복사합니다.

```bash
adb push stressapptest-android-arm64 /data/local/tmp/stressapptest
adb shell chmod 0755 /data/local/tmp/stressapptest
```

Qualcomm AOSS node를 변경하려면 root 권한과 허용된 SELinux domain이 필요합니다. `adb root`를 지원하는 `userdebug` 또는 `eng` build에서는 다음과 같이 실행할 수 있습니다.

```bash
adb root
adb shell
cd /data/local/tmp
```

상용 `user` build에서는 root 권한과 SELinux 정책이 별도로 준비되어야 합니다.

## 패턴 선택

패턴 번호는 0부터 시작합니다. 이 fork에서 ID `27`은 `OneZero256`입니다.

다음 두 명령은 동일하게 작동합니다.

```bash
./stressapptest \
  -M 1024 -m 4 -i 4 -s 600 \
  --printsec 10 \
  -P 27
```

```bash
./stressapptest \
  -M 1024 -m 4 -i 4 -s 600 \
  --printsec 10 \
  -P OneZero256
```

패턴 이름은 대소문자를 구분하지 않습니다. `-P`를 생략하면 upstream과 동일하게 가중치 기반 무작위 패턴 선택을 사용합니다.

lowercase `-p`는 기존 memory block 크기 옵션입니다. 패턴 선택에는 반드시 uppercase `-P`를 사용합니다.

여러 패턴은 쉼표로 연결합니다. 각 Worker가 새 block의 pattern을 선택할 때 목록의 다음 항목을 가져오며, 마지막 항목 다음에는 첫 항목으로 돌아갑니다.

```bash
./stressapptest \
  -M 1024 -m 4 -i 4 -s 600 \
  --printsec 10 \
  -P OneZero256,FiveA256,walkingOnes128
```

Pattern 선택 호출 순서는 보존됩니다. 여러 Fill Worker가 block을 병렬로 가져오므로 메모리 주소의 증가 순서와 pattern 순서는 일치하지 않을 수 있습니다. 이 기능은 각 pattern을 일정 시간씩 실행하는 단계 전환 방식이 아니라 block별 순환 배정 방식입니다.

## DDR 주파수 하나로 고정

OneZero256 패턴을 3196 요청값으로 10분 동안 실행합니다.

```bash
./stressapptest \
  -M 1024 -m 4 -i 4 -s 600 \
  --printsec 10 \
  -P OneZero256 \
  --ddr-freq 3196
```

## 전체 DDR 주파수 sweep

`all`에 등록된 값은 다음과 같습니다.

```text
547, 768, 1017, 1353, 1555, 1708, 2092, 2736, 3196, 4266, 5333
```

각 값을 3초씩 유지하면서 순서대로 반복합니다.

```bash
./stressapptest \
  -M 1024 -m 4 -i 4 -s 600 \
  --printsec 10 \
  -P OneZero256 \
  --ddr-freq all
```

## 선택한 DDR 주파수만 sweep

쉼표 사이에 공백을 넣지 않습니다.

```bash
./stressapptest \
  -M 1024 -m 4 -i 4 -s 600 \
  --printsec 10 \
  -P OneZero256 \
  --ddr-freq 547,1017,2092,3196,5333 \
  --ddr-step 3
```

`--ddr-step`은 선택 옵션이며 기본값은 3초입니다. `--ddr-freq`를 생략하면 DDR 제어 기능은 비활성화되고 AOSS node를 열거나 쓰지 않습니다. `--ddr-step`만 지정해도 DDR 주파수는 변경되지 않습니다.

## 로그 확인

주파수 값을 AOSS node에 성공적으로 쓰면 다음 로그가 기록됩니다. 초기 pattern 기록 전과 Worker 시작 전에 첫 값을 각각 쓰므로 같은 값이 두 번 표시됩니다.

```text
Log: DDR_FREQ write=3196 monotonic_us=... node=/sys/kernel/debug/aoss_send_message
```

Memory mismatch가 발생하면 세 시점의 DDR 값이 같은 오류 record에 저장됩니다.

```text
Hardware Error: miscompare on CPU 3(<-1) at ...: read:0x0000000000000000, reread:0xffffffff00000000 expected:0xffffffff00000000. 'OneZero256' read error. ddr_freq(write=3196 read=4266 reread=5333).
```

`write`는 해당 block의 expected data를 마지막으로 전체 기록하기 직전에 저장한 값, `read`는 mismatch가 발생한 최초 load 직전에 저장한 값, `reread`는 cache flush 뒤 다시 load하기 직전에 저장한 값입니다. Sweep 전환이 이 동작 사이에 발생하면 세 값이 서로 다를 수 있습니다.

각 숫자는 AOSS node에 마지막으로 성공적으로 쓴 내부 기록값이며 실제 DDR clock readback 결과가 아닙니다. DDR 제어를 사용하지 않았거나 write metadata가 없는 경로는 `unknown`으로 표시됩니다. 제품의 clock readback node가 있다면 별도 계측값과 함께 확인해야 합니다.

최초 read 뒤에는 한 번의 `CheckRegion()`에서 최대 128개 오류를 먼저 수집하고 각 주소를 순차적으로 reread합니다. 따라서 오류 출력이 많으면 `read`와 `reread` 사이에 다음 3초 sweep 전환이 발생할 수 있습니다. Logger queue에서 출력이 지연되어도 세 값은 각 동작 시점에 이미 record에 저장되어 있습니다.

## 직접 Android ARM64 빌드

Android NDK 경로를 지정하고 실행합니다. 기본 Android API level은 30입니다.

```bash
export ANDROID_NDK_HOME=/path/to/android-ndk
./scripts/build_android_arm64.sh 30
```

결과 파일은 다음 경로에 생성됩니다.

```text
out/android-arm64/stressapptest
```

빌드 스크립트는 `aarch64-linux-android` target, PIE, pthread, `STRESSAPPTEST_CPU_AARCH64`를 사용합니다. C++ runtime은 실행 파일에 정적으로 연결하므로 휴대폰에 `libc++_shared.so`를 별도로 복사하지 않습니다.

## GitHub Actions 자동 빌드와 Release

`.github/workflows/android-arm64-release.yml`은 다음 순서로 실행됩니다.

1. PR에서 Android AArch64 빌드 가능 여부를 확인합니다.
2. `master`에 소스 변경이 반영되면 해당 commit을 Android API 30용으로 빌드합니다.
3. ELF architecture가 AArch64인지 확인합니다.
4. `libc++_shared.so` 의존성이 없는지 확인합니다.
5. source commit으로 `android-<commit>` tag와 GitHub Release를 만듭니다.
6. 실행 파일, SHA-256 checksum, 압축 파일을 Release 자산으로 등록합니다.
7. README 상단 다운로드 버튼이 가장 최신 Release의 실행 파일을 내려받습니다.

GitHub Actions 화면에서 `Build Android ARM64 release` workflow를 수동 실행할 수도 있습니다. Release는 `master` commit에서 실행했을 때만 게시됩니다.

## 상세 설명서

기존 소스 구조와 worker, queue, cache, physical mapping 설명은 [StressAppTest 설명 사이트](https://stpcoder.github.io/stressapptest/)에서 확인할 수 있습니다.

License: Apache License 2.0

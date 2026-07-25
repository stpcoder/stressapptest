# StressAppTest Android ARM64 확장판

이 저장소는 공개 [stressapptest](https://github.com/stressapptest/stressapptest)에 Android AArch64 실장기 테스트 기능을 추가한 개인 fork입니다. 이 README는 이 fork에서 추가한 기능과 실행 방법만 설명합니다.

[![Android ARM64 최신 버전 다운로드](https://img.shields.io/badge/Android_ARM64-최신_버전_다운로드-1976d2?style=for-the-badge&logo=android&logoColor=white)](https://github.com/stpcoder/stressapptest/releases/latest/download/stressapptest-android-arm64)

압축 파일과 SHA-256 checksum이 필요하면 [최신 Release 페이지](https://github.com/stpcoder/stressapptest/releases/latest)에서 다운로드할 수 있습니다.

## 추가된 기능

| 기능 | 옵션 | 설명 |
|---|---|---|
| 패턴 고정 | `-P <ID\|이름>` | 모든 초기 memory block을 지정한 패턴으로 채우고 해당 패턴만 사용 |
| DDR 주파수 고정 | `--ddr <주파수>` | 초기 pattern 기록 전과 Worker 시작 전에 Qualcomm AOSS에 요청 |
| 전체 주파수 sweep | `--ddr-sweep all` | 등록된 전체 주파수를 순서대로 반복 |
| 선택 주파수 sweep | `--ddr-sweep <목록>` | 쉼표로 지정한 주파수만 순서대로 반복 |
| 전환 간격 | `--ddr-step <초>` | sweep 주파수 유지 시간. 기본값은 3초 |
| AOSS node 변경 | `--ddr-node <경로>` | 기본 debugfs node가 다른 제품에서 사용 |
| 오류 주파수 기록 | 자동 | `read`, `reread`, `expected` 오류에 mismatch 감지 시점의 요청 주파수 출력 |

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

## 패턴 하나만 실행

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

## DDR 주파수 하나로 고정

OneZero256 패턴을 3196 요청값으로 10분 동안 실행합니다.

```bash
./stressapptest \
  -M 1024 -m 4 -i 4 -s 600 \
  --printsec 10 \
  -P OneZero256 \
  --ddr 3196
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
  --ddr-sweep all \
  --ddr-step 3
```

## 선택한 DDR 주파수만 sweep

쉼표 사이에 공백을 넣지 않습니다.

```bash
./stressapptest \
  -M 1024 -m 4 -i 4 -s 600 \
  --printsec 10 \
  -P OneZero256 \
  --ddr-sweep 547,1017,2092,3196,5333 \
  --ddr-step 3
```

`--ddr`와 `--ddr-sweep`은 동시에 사용할 수 없습니다.

## 로그 확인

주파수 요청에 성공하면 다음 로그가 기록됩니다. 첫 값은 초기 pattern 기록 전과 Worker 시작 전에 각각 요청되므로 같은 값이 두 번 표시됩니다.

```text
Log: DDR_FREQ request=3196 monotonic_us=... node=/sys/kernel/debug/aoss_send_message
Log: DDR_FREQ active_request=3196 monotonic_us=...
```

Memory mismatch가 발생하면 처음 잘못된 값을 읽은 시점의 요청 주파수가 같은 오류 record에 저장됩니다.

```text
Hardware Error: miscompare on CPU 3(<-1) at ...: read:0x0000000000000000, reread:0xffffffff00000000 expected:0xffffffff00000000. 'OneZero256' read error. ddr_freq:3196(requested).
```

`ddr_freq:3196(requested)`는 AOSS에 마지막으로 성공적으로 전달한 요청값입니다. 실제 DDR clock이 3196에 도달했다는 readback 결과는 아닙니다. 제품의 clock readback node가 있다면 별도 계측값과 함께 확인해야 합니다.

주파수 전환 직후 발생한 오류도 로그 출력 시점의 주파수가 아니라 mismatch를 감지한 시점의 요청값으로 기록됩니다. 따라서 Logger queue가 밀려 나중에 출력되더라도 다음 주파수로 잘못 표시되지 않습니다.

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

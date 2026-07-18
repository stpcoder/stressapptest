#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"
android_api="${1:-${ANDROID_API:-30}}"
output_dir="${2:-${ANDROID_BUILD_DIR:-${repo_root}/out/android-arm64}}"

ndk_root="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"
if [[ -z "${ndk_root}" && -d "${HOME}/Library/Android/sdk/ndk" ]]; then
  ndk_root="$(find "${HOME}/Library/Android/sdk/ndk" -mindepth 1 -maxdepth 1 -type d | sort -V | tail -1)"
fi

if [[ -z "${ndk_root}" || ! -d "${ndk_root}" ]]; then
  echo "ANDROID_NDK_HOME 또는 ANDROID_NDK_ROOT를 Android NDK 경로로 설정하십시오." >&2
  exit 1
fi

case "$(uname -s)" in
  Darwin) host_tag="darwin-x86_64" ;;
  Linux) host_tag="linux-x86_64" ;;
  *)
    echo "지원하지 않는 build host입니다: $(uname -s)" >&2
    exit 1
    ;;
esac

toolchain="${ndk_root}/toolchains/llvm/prebuilt/${host_tag}"
cxx="${toolchain}/bin/aarch64-linux-android${android_api}-clang++"
if [[ ! -x "${cxx}" ]]; then
  echo "AArch64 Android compiler를 찾을 수 없습니다: ${cxx}" >&2
  exit 1
fi

mkdir -p "${output_dir}"
install -m 0644 \
  "${repo_root}/src/stressapptest_config_android.h" \
  "${output_dir}/stressapptest_config.h"

sources=(
  src/main.cc
  src/adler32memcpy.cc
  src/disk_blocks.cc
  src/error_diag.cc
  src/finelock_queue.cc
  src/logger.cc
  src/os.cc
  src/os_factory.cc
  src/pattern.cc
  src/queue.cc
  src/sat.cc
  src/sat_factory.cc
  src/worker.cc
)

source_paths=()
for source in "${sources[@]}"; do
  source_paths+=("${repo_root}/${source}")
done

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

echo "Android ARM64 binary: ${output_dir}/stressapptest"

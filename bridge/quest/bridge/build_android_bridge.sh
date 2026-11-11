#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build Fukami's PCSX2 GS bridge and the PCSX2 SPU2 core for Android arm64-v8a (Meta Quest).
# Re-runnable: configures (or reconfigures) and builds incrementally.
#
#   quest/bridge/build_android_bridge.sh            # build + check
#   BUILD_DIR=... JOBS=8 quest/bridge/build_android_bridge.sh -DLTO_PCSX2_CORE=ON
#
# Environment (all optional):
#   ANDROID_NDK      NDK root. Default: ~/workspace/android-ndk-r30, else
#                    ~/workspace/android-sdk/ndk/27.2.12479018.
#   CMAKE            cmake binary, >= 3.25 (shaderc's SPIRV-Tools does not configure with
#                    3.22.1). Default: ~/workspace/android-sdk/cmake/3.31.6/bin/cmake (the
#                    version ARMSX2's Gradle build pins) if installed, else cmake on PATH.
#                    CMake 4.x works: CMAKE_POLICY_VERSION_MINIMUM=3.5 is passed for the
#                    old 3rdparty cmake_minimum_required() floors.
#   NINJA            ninja binary. Default: the one next to CMAKE, else ninja on PATH.
#   BUILD_DIR        Default: <repo>/build-android-bridge.
#   JOBS             Parallel jobs. Default: 8.
#   ARMSX2_SOURCE    ARMSX2 + Fukami bridge patches checkout.
#                    Default: <repo>/build-deps/armsx2-pcsx2.
#   ARMSX2_UPSTREAM  Where to clone ARMSX2 from when ARMSX2_SOURCE is missing.
#                    Default: ~/workspace/ARMSX2-3D. The clone is checked out at the base
#                    commit recorded in quest/bridge/patches/BASE and the patches are
#                    applied with `git am` on a new branch fukami-bridge.
#   SPU2_SOURCE      Pinned PCSX2 2.8.2 checkout for the SPU2 core.
#                    Default: <repo>/build-deps/pcsx2-2.8.2.
#   ANDROID_PLATFORM Default: android-29.
# Extra arguments are passed to the CMake configure step.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(cd "$here/../.." && pwd)

ndk=${ANDROID_NDK:-}
if [[ -z "$ndk" ]]; then
    for candidate in "$HOME/workspace/android-ndk-r30" "$HOME/workspace/android-sdk/ndk/27.2.12479018"; do
        if [[ -f "$candidate/build/cmake/android.toolchain.cmake" ]]; then
            ndk=$candidate
            break
        fi
    done
fi
if [[ -z "$ndk" || ! -f "$ndk/build/cmake/android.toolchain.cmake" ]]; then
    echo "error: no Android NDK found; set ANDROID_NDK" >&2
    exit 1
fi

cmake_bin=${CMAKE:-}
if [[ -z "$cmake_bin" ]]; then
    if [[ -x "$HOME/workspace/android-sdk/cmake/3.31.6/bin/cmake" ]]; then
        cmake_bin="$HOME/workspace/android-sdk/cmake/3.31.6/bin/cmake"
    else
        cmake_bin=$(command -v cmake)
    fi
fi
ninja_bin=${NINJA:-}
if [[ -z "$ninja_bin" ]]; then
    if [[ -x "$(dirname "$cmake_bin")/ninja" ]]; then
        ninja_bin="$(dirname "$cmake_bin")/ninja"
    else
        ninja_bin=$(command -v ninja || echo "$HOME/.local/bin/ninja")
    fi
fi
cmake_major=$("$cmake_bin" --version | sed -n 's/^cmake version \([0-9]*\)\..*/\1/p')
policy_args=()
if (( cmake_major >= 4 )); then
    policy_args=(-DCMAKE_POLICY_VERSION_MINIMUM=3.5)
fi

build_dir=${BUILD_DIR:-"$root/build-android-bridge"}
jobs=${JOBS:-8}
armsx2=${ARMSX2_SOURCE:-"$root/build-deps/armsx2-pcsx2"}
armsx2_upstream=${ARMSX2_UPSTREAM:-"$HOME/workspace/ARMSX2-3D"}
spu2_source=${SPU2_SOURCE:-"$root/build-deps/pcsx2-2.8.2"}
platform=${ANDROID_PLATFORM:-android-29}

# --- ARMSX2 + bridge patches ------------------------------------------------------------
if [[ ! -d "$armsx2/.git" ]]; then
    base=$(tr -d '[:space:]' < "$here/patches/BASE")
    echo "== cloning ARMSX2 from $armsx2_upstream at $base into $armsx2"
    git clone --no-hardlinks "$armsx2_upstream" "$armsx2"
    git -C "$armsx2" checkout -b fukami-bridge "$base"
    git -C "$armsx2" -c user.name="${GIT_AUTHOR_NAME:-Sgt.Bilko}" \
        -c user.email="${GIT_AUTHOR_EMAIL:-59089219+SgtBilko76@users.noreply.github.com}" \
        am --committer-date-is-author-date "$here"/patches/0*.patch
fi
if ! grep -q GSSnapshotLocalMemory "$armsx2/pcsx2/GS/GS.h"; then
    echo "error: $armsx2 has no Fukami bridge patches (expected branch fukami-bridge)" >&2
    exit 1
fi

# shaderc's SPIRV-Tools / SPIRV-Headers / glslang are fetched on demand (ARMSX2 CI does the
# same with shaderc's utils/git-sync-deps).
if [[ ! -d "$armsx2/platforms/android/app/src/main/cpp/3rdparty/shaderc/third_party/spirv-tools" ]]; then
    "$here/fetch_shaderc_deps.sh" "$armsx2"
fi

# --- configure + build ----------------------------------------------------------------
# Release binaries must not carry the builder's directory names.
prefix_map="-ffile-prefix-map=$armsx2=/pcsx2 -ffile-prefix-map=$root=/fukami -ffile-prefix-map=$build_dir=/bridge-build"
if [[ -n "$spu2_source" ]]; then
    prefix_map+=" -ffile-prefix-map=$spu2_source=/pcsx2-spu2"
fi
# -gline-tables-only: symbolised native crash backtraces (tombstones) at a small size cost;
# the APK packaging step strips the libraries anyway.
cflags="-O3 -gline-tables-only $prefix_map"

echo "== NDK $ndk ($(sed -n 's/^Pkg.Revision = //p' "$ndk/source.properties"))"
echo "== cmake $cmake_bin, ninja $ninja_bin, build dir $build_dir, -j$jobs"
"$cmake_bin" -S "$here" -B "$build_dir" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$ninja_bin" \
    -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM="$platform" \
    -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="$cflags" \
    -DCMAKE_CXX_FLAGS="$cflags" \
    -DARMSX2_ANDROID_HOST_PAGE_SIZE=0x1000 \
    -DLTO_PCSX2_CORE=OFF \
    -DRRV_ARMSX2_SOURCE_DIR="$armsx2" \
    -DRRV_PCSX2_SPU2_SOURCE_DIR="$spu2_source" \
    ${policy_args[@]+"${policy_args[@]}"} \
    ${@+"$@"}

"$cmake_bin" --build "$build_dir" -j "$jobs" --target \
    rrv-pcsx2-gs-bridge rrv_pcsx2_spu2 rrv_pcsx2_spu2_shared spu2_selftest

# --- check ----------------------------------------------------------------------------
python3 "$root/quest/tools/check_android_so.py" --ndk "$ndk" \
    --bridge "$build_dir/pcsx2-gs-bridge/librrv-pcsx2-gs-bridge.so" \
    --bridge-consumer "$root/src/gs-backend/rrv_gs_backend.cpp" \
    --bridge-consumer "$root/src/host/rrv_sdl_presentation.cpp" \
    --spu2 "$build_dir/pcsx2-spu2/librrv_pcsx2_spu2.so" \
    --spu2-header "$root/tools/pcsx2-spu2/rrv_pcsx2_spu2.h"

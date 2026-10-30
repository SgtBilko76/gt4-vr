#!/bin/bash
# Build the GT4 VR APK: libgt4vr.so (game code + runtime + Quest host, NDK, outside Gradle),
# stage it with the PCSX2 GS bridge, its resources and the OpenXR loader, then let Gradle package.
#
# usage: quest/tools/build_quest.sh [--no-apk]
#   JOBS=N        parallel compile jobs (default 4: generated units take ~1 GB each)
#   BRIDGE_BUILD  PCSX2 GS bridge build dir (default: the Ridge Racer V port's Android bridge)
#
# The APK holds the game's recompiled code: never publish it.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
NDK=${ANDROID_NDK:-$HOME/workspace/android-ndk-r30}
SDK_CMAKE=$(ls -d "$HOME"/workspace/android-sdk/cmake/*/bin | tail -1)
BUILD="$ROOT/build/android-gt4"
BRIDGE_BUILD=${BRIDGE_BUILD:-$HOME/Projekte/fukami-wt-bridge/build-android-bridge}
STAGE="$ROOT/quest/android/build/stage"
STRIP="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"

"$SDK_CMAKE/cmake" -S "$ROOT" -B "$BUILD" -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$SDK_CMAKE/ninja" \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static \
  -DCMAKE_BUILD_TYPE=Release \
  -DPS2_GENERATED="$ROOT/generated/gt4" \
  -DPS2_OPENXR_DIR="$ROOT/quest/third_party/openxr" > "$BUILD.configure.log" 2>&1 || { tail -30 "$BUILD.configure.log"; exit 1; }
"$SDK_CMAKE/ninja" -C "$BUILD" -j "${JOBS:-4}" gt4vr

# Undefined symbols not provided by system libraries mean a dlopen failure on the headset.
"$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-nm" -D --undefined-only "$BUILD/libgt4vr.so" \
  | grep -v -E '@(LIBC|LIBDL|LIBM|LIBLOG|LIBANDROID|LIBEGL|LIBGLESV3|LIBAAUDIO)|^ *w ' \
  | grep -v -E ' (xr[A-Z]|A[A-Z][a-z]+_|egl[A-Z]|gl[A-Z]|__)' || true

rm -rf "$STAGE"
mkdir -p "$STAGE/jniLibs/arm64-v8a" "$STAGE/assets/gt4vr"
for lib in "$BUILD/libgt4vr.so" "$BRIDGE_BUILD/pcsx2-gs-bridge/librrv-pcsx2-gs-bridge.so" \
           "$ROOT/quest/third_party/openxr/lib/arm64-v8a/libopenxr_loader.so"; do
  "$STRIP" --strip-unneeded -o "$STAGE/jniLibs/arm64-v8a/$(basename "$lib")" "$lib"
done
cp -r "$BRIDGE_BUILD/pcsx2-gs-bridge/resources" "$STAGE/assets/gt4vr/resources"
python3 - "$STAGE/assets/gt4vr" <<'EOF'
import hashlib, pathlib, sys
root = pathlib.Path(sys.argv[1])
files = sorted(p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_file())
digest = hashlib.sha256()
for rel in files:
    digest.update(rel.encode() + b"\0" + (root / rel).read_bytes())
(root / "manifest.txt").write_text("\n".join(files) + "\n")
(root / "stamp.txt").write_text(digest.hexdigest()[:16])
print(f"assets: {len(files)} files, stamp {digest.hexdigest()[:16]}")
EOF
ls -la "$STAGE/jniLibs/arm64-v8a"

[ "${1:-}" = "--no-apk" ] && exit 0
cd "$ROOT/quest/android"
[ -f local.properties ] || echo "sdk.dir=$HOME/workspace/android-sdk" > local.properties
JAVA_HOME=$(ls -d "$HOME"/tools/jdk-17* | head -1) ./gradlew --no-daemon -q --offline assembleRelease
ls -la app/build/outputs/apk/release/

#!/bin/bash
# Build the GT4 VR APK: libgt4vr.so (game code + runtime + Quest host, NDK, outside Gradle),
# stage it with the PCSX2 GS bridge, its resources and the OpenXR loader, then let Gradle package.
#
# usage: quest/tools/build_quest.sh [--no-apk]
#   GAME=gt4|gt4r gt4 = A-Spec II (online.portroyale.gt4vr, /sdcard/GT4VR, default),
#                 gt4r = retail USA v2.00 with LAN play (online.portroyale.gt4online "GT4 Online", /sdcard/GT4Online)
#   VERSION_NAME  versionName (default 0.1.0-beta), VERSION_CODE versionCode (default 2)
#   JOBS=N        parallel compile jobs (default 4: generated units take ~1 GB each)
#   BRIDGE_BUILD  PCSX2 GS bridge build dir (default: the Ridge Racer V port's Android bridge)
#
# The APK holds the game's recompiled code: never publish it.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
NDK=${ANDROID_NDK:-$HOME/workspace/android-ndk-r30}
SDK_CMAKE=$(ls -d "$HOME"/workspace/android-sdk/cmake/*/bin | tail -1)
GAME=${GAME:-gt4}
case "$GAME" in
  gt4)  variant=(-DGT4VR_ONLINE=OFF); APP_ID=online.portroyale.gt4vr; APP_LABEL="GT4 Spec II HD 3D" ;;
  # Retail USA v2.00 "Gran Turismo 4 Online VR" (was "GT4 HD 3D") (owner 2026-11-05: the public build, online through his relay:
  # online lobby, leaderboard reports, ad hoc Multi-LAN Race; 3D; no HD texture pack since 2026-11-07).
  # The old LAN app (online.portroyale.gt4online) is GAME=gt4online.
  gt4r) variant=(-DGT4VR_DATA_DIR=/sdcard/GT4HD -DGT4VR_OLD_DATA_DIR= -DGT4VR_TITLE="Gran Turismo 4 Online VR"
                 -DGT4VR_NET_PRODUCT=SCUS97328 -DGT4VR_CAMERA_HOOKS=OFF -DGT4VR_ONLINE=ON)
        APP_ID=online.portroyale.gt4hd; APP_LABEL="Gran Turismo 4 Online VR" ;;
  # Spec II 1.10 (owner 2026-11-06): the user brings his own patched ISO (any *.iso in /sdcard/GT4SpecII),
  # the app checks the program's CRC. Offline only (owner: no network play options in Spec II; the
  # beta's LAN mode hangs anyway). Goal: a polished single-player version.
  gt4s) variant=(-DGT4VR_DATA_DIR=/sdcard/GT4SpecII -DGT4VR_OLD_DATA_DIR= -DGT4VR_TITLE="Gran Turismo 4 Spec II VR"
                 -DGT4VR_NET_PRODUCT=SPEC2V110 -DGT4VR_CAMERA_HOOKS=ON -DGT4VR_ONLINE=OFF -DGT4VR_RELAY=OFF
                 -DGT4VR_ELF_CRC=646B2E29)
        APP_ID=online.portroyale.gt4vr; APP_LABEL="Gran Turismo 4 Spec II VR" ;;
  gt4online) variant=(-DGT4VR_DATA_DIR=/sdcard/GT4Online -DGT4VR_OLD_DATA_DIR= -DGT4VR_TITLE="GT4 Online"
                 -DGT4VR_NET_PRODUCT=SCUS97328 -DGT4VR_CAMERA_HOOKS=OFF -DGT4VR_ONLINE=ON)
        APP_ID=online.portroyale.gt4online; APP_LABEL="GT4 Online"; GAME=gt4r ;;
  *) echo "unknown GAME=$GAME" >&2; exit 2 ;;
esac
BUILD="$ROOT/build/android-$GAME"
BRIDGE_BUILD=${BRIDGE_BUILD:-$HOME/Projekte/fukami-wt-bridge/build-android-bridge}
STAGE="$ROOT/quest/android/build/stage"
STRIP="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"

"$SDK_CMAKE/cmake" -S "$ROOT" -B "$BUILD" -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$SDK_CMAKE/ninja" \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static \
  -DCMAKE_BUILD_TYPE=Release \
  -DPS2_GENERATED="$ROOT/generated/$GAME" "${variant[@]}" \
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
# Shader pipeline seed (quest/tools/quest_pull_pipelines.sh): built up front on a fresh install.
if [ -f "$ROOT/quest/assets/rrv_tfx_pipelines.bin" ]; then
  mkdir -p "$STAGE/assets/gt4vr/pipelines"
  cp "$ROOT/quest/assets/rrv_tfx_pipelines.bin" "$STAGE/assets/gt4vr/pipelines/"
fi
# Compiled shader seed (same tool): PCSX2's SPIR-V + driver pipeline cache for a fresh install.
if [ -f "$ROOT/quest/assets/shader-cache/vulkan_shaders.idx" ]; then
  mkdir -p "$STAGE/assets/gt4vr/shader-cache"
  cp "$ROOT"/quest/assets/shader-cache/vulkan_* "$STAGE/assets/gt4vr/shader-cache/"
fi
# HD texture packs (A-Spec II only, private build): TEXTURES=<dir> with <serial>/replacements/... (default
# quest/assets/textures, symlinks allowed), staged as assets/textures + its own manifest/stamp; stamp =
# file names + sizes, so the headset copies them again only when the packs change. TEXTURES=none skips.
# An APK holds at most 65535 zip entries and the packs have ~90k files, so they go into a few bundle
# files (assets/textures/bundle-NN.bin, <= 64 MB each: Gradle reads each asset whole): textures-manifest.txt lists
# "bundle<TAB>offset<TAB>size<TAB>path" per file. Only .ktx/.png/.dds are packed.
TEXTURES=${TEXTURES:-$ROOT/quest/assets/textures}
if [ "$TEXTURES" != none ] && [ -d "$TEXTURES" ]; then
  mkdir -p "$STAGE/assets/textures"
  python3 - "$TEXTURES" "$STAGE/assets" <<'EOF'
import hashlib, os, pathlib, sys
src = pathlib.Path(sys.argv[1]).resolve(); assets = pathlib.Path(sys.argv[2])
files = sorted(p.relative_to(src).as_posix() for p in src.rglob("*")
               if p.is_file() and p.suffix.lower() in (".ktx", ".png", ".dds"))
digest = hashlib.sha256(); limit = 64 << 20
lines, bundle, offset, out, total = [], 0, 0, None, 0
for rel in files:
    data = (src / rel).read_bytes()
    if out is None or offset + len(data) > limit:
        if out: out.close()
        bundle += 0 if out is None else 1
        out = open(assets / "textures" / f"bundle-{bundle:02d}.bin", "wb"); offset = 0
    out.write(data)
    lines.append(f"{bundle}\t{offset}\t{len(data)}\t{rel}")
    digest.update(rel.encode() + b"\0" + str(len(data)).encode() + b"\n")
    offset += len(data); total += len(data)
out.close()
(assets / "textures-manifest.txt").write_text("\n".join(lines) + "\n")
(assets / "textures-stamp.txt").write_text(digest.hexdigest()[:16])
print(f"textures: {len(files)} files in {bundle + 1} bundles, {total / 2**30:.2f} GB, stamp {digest.hexdigest()[:16]}")
EOF
fi
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
JAVA_HOME=$(ls -d "$HOME"/tools/jdk-17* | head -1) ./gradlew --no-daemon -q --offline assembleRelease -PgtAppId="$APP_ID" -PgtAppLabel="$APP_LABEL" \
  -PgtVersionName="${VERSION_NAME:-0.1.0-beta}" -PgtVersionCode="${VERSION_CODE:-2}"
mkdir -p "$ROOT/build/apk"
cp app/build/outputs/apk/release/app-release.apk "$ROOT/build/apk/$GAME.apk"
ls -la app/build/outputs/apk/release/ "$ROOT/build/apk/$GAME.apk"

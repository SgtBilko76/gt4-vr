#!/bin/bash
# Build the GT4 VR APK: libgt4vr.so (game code + runtime + Quest host, NDK, outside Gradle),
# stage it with the PCSX2 GS bridge, its resources and the OpenXR loader, then let Gradle package.
#
# usage: quest/tools/build_quest.sh [--no-apk]
#   GAME=gt4vr    "Gran Turismo 4 VR": Spec II 1.10 (/sdcard/GT4SpecII) + retail v2.00 online (/sdcard/GT4HD)
#                 in one APK, selector at start (default). gt4s / gt4r = one version only.
#   VERSION_NAME  versionName (default 0.1.0-beta), VERSION_CODE versionCode (default 2)
#   JOBS=N        parallel compile jobs (default 4: generated units take ~1 GB each)
#   BRIDGE_BUILD  PCSX2 GS bridge build dir (default: the Ridge Racer V port's Android bridge)
#
# The APK holds the game's recompiled code: never publish it.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
NDK=${ANDROID_NDK:-$HOME/workspace/android-ndk-r30}
SDK_CMAKE=$(ls -d "$HOME"/workspace/android-sdk/cmake/*/bin | tail -1)
GAME=${GAME:-gt4vr}
# variant <game>: compile flags, native library name and build dir of one game version.
variant() {
  case "$1" in
  gt4)  VARIANT=(-DGT4VR_ONLINE=OFF -DGT4VR_LIB_NAME=gt4vr_specii); LIBNAME=gt4vr_specii; SRC=gt4 ;;
  # Retail USA v2.00 (owner 2026-11-05: online through his relay: lobby panel, leaderboard reports, ad hoc
  # Multi-LAN Race; 3D; no HD texture pack). The user's own ISO in /sdcard/GT4HD; the program is read from
  # it and checked (CRC A9D9C0A2), like Spec II.
  gt4r) VARIANT=(-DGT4VR_DATA_DIR=/sdcard/GT4HD -DGT4VR_OLD_DATA_DIR= -DGT4VR_TITLE="Gran Turismo 4 3D/VR (Online)"
                 -DGT4VR_NET_PRODUCT=SCUS97328 -DGT4VR_CAMERA_HOOKS=ON -DGT4VR_ONLINE=ON
                 -DGT4VR_ELF_CRC=A9D9C0A2 "-DGT4VR_DISC_NAME=Gran Turismo 4 (USA) v2.00" -DGT4VR_LIB_NAME=gt4vr_online)
        LIBNAME=gt4vr_online; SRC=gt4r ;;
  # Spec II 1.10 (owner 2026-11-06): the user brings his own patched ISO (any *.iso in /sdcard/GT4SpecII),
  # the app checks the program's CRC. Offline only (no network play options in Spec II).
  gt4s) VARIANT=(-DGT4VR_DATA_DIR=/sdcard/GT4SpecII -DGT4VR_OLD_DATA_DIR= -DGT4VR_TITLE="Gran Turismo 4 3D/VR (Spec II)"
                 -DGT4VR_NET_PRODUCT=SPEC2V110 -DGT4VR_CAMERA_HOOKS=ON -DGT4VR_ONLINE=OFF -DGT4VR_RELAY=OFF
                 -DGT4VR_ELF_CRC=646B2E29 "-DGT4VR_DISC_NAME=Gran Turismo 4 Spec II 1.10" -DGT4VR_LIB_NAME=gt4vr_specii)
        LIBNAME=gt4vr_specii; SRC=gt4s ;;
  *) echo "unknown GAME=$1" >&2; exit 2 ;;
  esac
}
# "Gran Turismo 4 VR" (owner 2026-11-08): ONE app with both versions; a 2D selector at start picks one
# (quest/android LauncherActivity). Package online.portroyale.gt4vr (the Spec II app's, keeps its saves).
# GAME=gt4s / gt4r build an app with only that version (the selector then starts it directly).
case "$GAME" in
  gt4vr) GAMES=(gt4s gt4r); APP_ID=online.portroyale.gt4vr; APP_LABEL="Gran Turismo 4 3D/VR" ;;
  gt4s)  GAMES=(gt4s); APP_ID=online.portroyale.gt4vr; APP_LABEL="Gran Turismo 4 3D/VR" ;;
  gt4r)  GAMES=(gt4r); APP_ID=online.portroyale.gt4hd; APP_LABEL="Gran Turismo 4 Online VR" ;;
  *) echo "unknown GAME=$GAME (gt4vr, gt4s, gt4r)" >&2; exit 2 ;;
esac
BRIDGE_BUILD=${BRIDGE_BUILD:-$HOME/Projekte/fukami-wt-bridge/build-android-bridge}
STAGE="$ROOT/quest/android/build/stage"
STRIP="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"

LIBS=()
for g in "${GAMES[@]}"; do
  variant "$g"
  BUILD="$ROOT/build/android-$g"
  "$SDK_CMAKE/cmake" -S "$ROOT" -B "$BUILD" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$SDK_CMAKE/ninja" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release \
    -DPS2_GENERATED="$ROOT/generated/$SRC" "${VARIANT[@]}" \
    -DPS2_OPENXR_DIR="$ROOT/quest/third_party/openxr" > "$BUILD.configure.log" 2>&1 || { tail -30 "$BUILD.configure.log"; exit 1; }
  "$SDK_CMAKE/ninja" -C "$BUILD" -j "${JOBS:-4}" gt4vr
  lib="$BUILD/lib$LIBNAME.so"
  # Undefined symbols not provided by system libraries mean a dlopen failure on the headset.
  "$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-nm" -D --undefined-only "$lib" \
    | grep -v -E '@(LIBC|LIBDL|LIBM|LIBLOG|LIBANDROID|LIBEGL|LIBGLESV3|LIBAAUDIO)|^ *w ' \
    | grep -v -E ' (xr[A-Z]|A[A-Z][a-z]+_|egl[A-Z]|gl[A-Z]|__)' || true
  LIBS+=("$lib")
done

rm -rf "$STAGE"
mkdir -p "$STAGE/jniLibs/arm64-v8a" "$STAGE/assets/gt4vr"
for lib in "${LIBS[@]}" "$BRIDGE_BUILD/pcsx2-gs-bridge/librrv-pcsx2-gs-bridge.so" \
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

#!/usr/bin/env bash
# Take the owner's GT4 shader collection off the Quest into quest/assets/, the seed build_quest.sh packs
# into both APKs (GT4 VR = /sdcard/GT4VR, GT4 Online = /sdcard/GT4Online):
#   rrv_tfx_pipelines.bin  pipeline list (mirrored by RRV_PIPELINE_EXPORT): both apps' lists and the old
#                          seed are merged, so a fresh install builds every pipeline either app has seen.
#   shader-cache/          PCSX2's compiled shaders + driver pipeline cache (exported at app start by
#                          quest_main.cpp): taken from the app whose cache is larger. Only valid for the
#                          same GPU driver; PCSX2 drops it on another one, so it can only help.
# Each app exports its cache at its next start, so start the app once after a session before pulling.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
adb="timeout 120 $HOME/workspace/android-sdk/platform-tools/adb -s ${QUEST_SERIAL:-192.168.178.115:5555}"
dst="$root/quest/assets"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$dst"
for app in GT4VR GT4Online; do
  mkdir -p "$tmp/$app"
  $adb pull /sdcard/$app/rrv_tfx_pipelines.bin "$tmp/$app/" >/dev/null 2>&1 || true
  $adb pull /sdcard/$app/shader-cache "$tmp/$app/" >/dev/null 2>&1 || true
done
python3 - "$tmp" "$dst" <<'EOF'
import os, shutil, struct, sys
tmp, dst = sys.argv[1], sys.argv[2]
def read(p):
    b = open(p, 'rb').read()
    magic, size, _ = struct.unpack('<IIQ', b[:16])
    assert magic == 0x31505252 and size == 32, (p, hex(magic), size)
    return b[:16], {b[i:i + 32] for i in range(16, len(b) - 31, 32)}
seed = os.path.join(dst, 'rrv_tfx_pipelines.bin')
hdr, merged = read(seed) if os.path.exists(seed) else (None, set())
before = len(merged)
for app in ('GT4VR', 'GT4Online'):
    p = os.path.join(tmp, app, 'rrv_tfx_pipelines.bin')
    if os.path.exists(p):
        h, s = read(p)
        hdr = hdr or h
        print(f"{app}: {len(s)} pipelines, {len(s - merged)} new")
        merged |= s
if hdr:
    with open(seed, 'wb') as f:
        f.write(hdr)
        for s in sorted(merged):
            f.write(s)
print(f"pipeline seed: {before} -> {len(merged)}")
names = ('vulkan_shaders.idx', 'vulkan_shaders.bin', 'vulkan_pipelines.bin')
best, best_size = None, 0
for app in ('GT4VR', 'GT4Online'):
    d = os.path.join(tmp, app, 'shader-cache')
    if all(os.path.exists(os.path.join(d, n)) for n in names):
        size = sum(os.path.getsize(os.path.join(d, n)) for n in names)
        print(f"{app}: shader cache {size // 1024} KB")
        if size > best_size:
            best, best_size = d, size
if best:
    out = os.path.join(dst, 'shader-cache')
    os.makedirs(out, exist_ok=True)
    for n in names:
        shutil.copyfile(os.path.join(best, n), os.path.join(out, n))
    print(f"shader cache seed <- {best.split(os.sep)[-2]} ({best_size // 1024} KB)")
else:
    print("no shader cache exported yet (start an app once with this build)")
EOF

#!/usr/bin/env bash
# Take the owner's GT4 shader pipeline collection off the Quest (/sdcard/GT4VR/rrv_tfx_pipelines.bin,
# mirrored there by RRV_PIPELINE_EXPORT) into quest/assets/, the seed stage_apk.py packs into the APK.
# Keeps the larger list when the device's one is smaller (a fresh install starts from the seed).
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
adb="timeout 60 $HOME/workspace/android-sdk/platform-tools/adb -s ${QUEST_SERIAL:-192.168.178.115:5555}"
dst="$root/quest/assets/rrv_tfx_pipelines.bin"
tmp=$(mktemp)
$adb pull /sdcard/GT4VR/rrv_tfx_pipelines.bin "$tmp" >/dev/null
python3 - "$tmp" "$dst" <<'EOF'
import sys, os, shutil, struct
src, dst = sys.argv[1], sys.argv[2]
def read(p):
    b = open(p, 'rb').read()
    magic, size, _ = struct.unpack('<IIQ', b[:16])
    assert magic == 0x31505252 and size == 32, (hex(magic), size)
    return b[:16], {b[i:i + 32] for i in range(16, len(b) - 31, 32)}
hdr, new = read(src)
old = read(dst)[1] if os.path.exists(dst) else set()
merged = new | old
os.makedirs(os.path.dirname(dst), exist_ok=True)
with open(dst, 'wb') as f:
    f.write(hdr)
    for s in sorted(merged):
        f.write(s)
print(f"device {len(new)}, seed before {len(old)}, seed now {len(merged)} pipelines -> {dst}")
EOF
rm -f "$tmp"

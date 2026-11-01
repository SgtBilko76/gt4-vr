#!/usr/bin/env bash
# Which sub-call of GT4's race renderer (0x1EB0F8) draws what: runs the Nurburgring race state once
# per variant with PS2_GT4_SKIP=<function entry> (quest/host/gt4_camera.cpp), captures the GS draws of
# one race frame (PS2_CAPTURE draws.tsv) and compares each variant with the base run
# (quest/tools/renderer_attrib.py).
#   quest/tools/renderer_attrib.sh [frame] addr...       e.g. renderer_attrib.sh 6050 29e038 1ef620
# Needs $SCRATCH/race.state and $SCRATCH/mv.ps2 (games/gt4/states). JOBS parallel runs (default 4).
set -u
cd "$(dirname "$0")/../.."
S=${SCRATCH:-$HOME/.hermes/cache/scratch/attrib}
frame=${1:-6050}
shift
mkdir -p "$S"
run() { # name skip
  local name=$1 skip=$2 d="$S/$1"
  rm -rf "$d" && mkdir -p "$d"
  PS2_GT4_SKIP="$skip" PS2_CAPTURE="$frame,$d,1" nice timeout 900 build/native-gt4/ps2run games/gt4/core.elf \
    --disc games/gt4/disc.iso --memcard "$S/mv.ps2" --load-state "$S/race.state" --run $((frame - 5896 + 3)) \
    --press 5900:start:4 --press 6000:cross:1400 --frames-dir "$d" --frame-every "$frame" > "$d.log" 2>&1
}
names=(base)
run base "" &
for a in "$@"; do
  while [ "$(jobs -rp | wc -l)" -ge "${JOBS:-4}" ]; do sleep 2; done
  names+=("s$a")
  run "s$a" "$a" &
done
wait
python3 quest/tools/renderer_attrib.py "$S" "${names[@]}"

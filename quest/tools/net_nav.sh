#!/bin/bash
# Menu navigation on the PC: load a state, press buttons (frame offsets relative to the state),
# save the last picture and a new state.
#   quest/tools/net_nav.sh in.state out-name frames "+30:cross:4 +90:down:4 ..." [extra env/args]
# Env: GAME (gt4 = A-Spec II, gt4r = retail v2.00), NAV_DIR (default ~/.hermes/cache/scratch/net), NAV_MC (memory card), PS2_NET_* passed through.
set -e
cd "$(dirname "$0")/../.."
D=${NAV_DIR:-$HOME/.hermes/cache/scratch/net}
in=$1; out=$2; frames=$3; presses=$4; shift 4
MC=${NAV_MC:-$D/a.ps2}
G=${GAME:-gt4}
start=$(cat "$in.frame" 2>/dev/null || echo 0)
args=()
for p in $presses; do
  off=${p%%:*}; rest=${p#*:}
  args+=(--press "$((start + ${off#+})):$rest")
done
end=$((start + frames))
rm -rf "$D/f-$out"; mkdir -p "$D/f-$out"
PS2_FRAMES_FROM=$((end - 3)) timeout 900 nice build/native-$G/ps2run games/$G/core.elf --disc games/$G/disc.iso --memcard "$MC" \
  --load-state "$in" --run "$frames" "${args[@]}" --frames-dir "$D/f-$out" --frame-every 1 \
  --save-state "$((end - 2)):$D/$out.state" "$@" > "$D/$out.log" 2>&1 || true
echo $((end - 2)) > "$D/$out.state.frame"
last=$(ls "$D/f-$out" | tail -1)
ffmpeg -v error -y -i "$D/f-$out/$last" "$D/$out.png"
echo "$D/$out.png (frame $end)"

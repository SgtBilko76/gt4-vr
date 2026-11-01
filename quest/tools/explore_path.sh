#!/bin/bash
# Finds code the static pass missed along one scripted path: run from a state with presses,
# add every missed entry point, translate, rebuild, repeat until the run gets through.
#   quest/tools/explore_path.sh <game> <state> <frames> "<+rel:button:hold ...>" [rounds] [-- extra ps2run args]
set -uo pipefail
cd "$(dirname "$0")/../.."
game=$1; state=$2; frames=$3; presses=$4; rounds=${5:-15}; shift 5 || shift $#
[ "${1:-}" = "--" ] && shift
D=${NAV_DIR:-$HOME/.hermes/cache/scratch/net}
start=$(cat "$state.frame")
args=()
for p in $presses; do off=${p%%:*}; args+=(--press "$((start + ${off#+})):${p#*:}"); done
for ((r = 1; r <= rounds; r++)); do
  rm -f "$D/explore-misses.txt"
  timeout 900 nice build/native-$game/ps2run games/$game/core.elf --disc games/$game/disc.iso \
    --memcard "${NAV_MC:-$D/a.ps2}" --load-state "$state" --run "$frames" "${args[@]}" \
    --misses "$D/explore-misses.txt" "$@" > "$D/explore-path.log" 2>&1
  end=$(grep -a "stopped after" "$D/explore-path.log" | tail -1)
  new=$(python3 - "games/$game/entries.txt" "$D/explore-misses.txt" <<'EOF'
import os, sys
entries, misses = sys.argv[1], sys.argv[2]
def token(t):
    name, _, at = t.rpartition(':')
    return (name + ':' if name else '') + '%x' % int(at, 16)
known = {token(t) for t in open(entries).read().split()} if os.path.exists(entries) else set()
found = [token(t) for t in open(misses).read().split()] if os.path.exists(misses) else []
new = sorted({t for t in found if t not in known})
with open(entries, 'a') as out:
    for t in new: out.write(t + '\n')
print(' '.join(new))
EOF
)
  echo "round $r: ${end:-?} | new: ${new:-none}"
  [ -n "$new" ] || break
  build/tools/eerecomp games/$game/core.elf --entries games/$game/entries.txt --out generated/$game | tail -1
  nice ninja -C build/native-$game -j${JOBS:-4} ps2run > "$D/explore-ninja.log" 2>&1 || { tail -20 "$D/explore-ninja.log"; exit 1; }
done

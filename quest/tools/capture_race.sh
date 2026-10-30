#!/usr/bin/env bash
# Records the GS draws of one race frame (PS2_CAPTURE draws.tsv) from the Nurburgring race state.
#   quest/tools/capture_race.sh name frame [extra ps2run args...]
# e.g. quest/tools/capture_race.sh hud 6300
#      quest/tools/capture_race.sh pause 6330 --press 6250:start:4
set -u
cd "$(dirname "$0")/../.."
S=${SCRATCH:-$HOME/.hermes/cache/scratch}
name=$1 frame=$2
shift 2
rm -rf "$S/cap-$name" && mkdir -p "$S/cap-$name"
PS2_CAPTURE="$frame,$S/cap-$name,1" nice timeout 900 build/native-gt4/ps2run games/gt4/core.elf --disc games/gt4/disc.iso \
  --memcard "$S/mv.ps2" --load-state "$S/race.state" --run $((frame - 5896 + 3)) --press 5900:start:4 \
  --press 6000:cross:1400 --frames-dir "$S/cap-$name" --frame-every "$frame" "$@" > "$S/cap-$name.log" 2>&1
echo "$name rc=$? draws: $(wc -l < "$S/cap-$name/draws.tsv")"

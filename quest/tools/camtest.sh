#!/usr/bin/env bash
# PC check of the VR camera hooks (quest/host/gt4_camera.cpp): runs the Nurburgring race state in
# ps2run with test head poses in parallel and saves frame 7000 of each.
#   quest/tools/camtest.sh name:VAR=VALUE[,VAR=VALUE] ...
# e.g. quest/tools/camtest.sh y0:A=1 y30:PS2_GT4_TEST_YAW=30
set -u
cd "$(dirname "$0")/../.."
S=${SCRATCH:-$HOME/.hermes/cache/scratch}
RUN=${RUN:-1110}
pids=()
for spec in "$@"; do
  name=${spec%%:*}
  vars=${spec#*:}
  rm -rf "$S/$name" && mkdir -p "$S/$name"
  # shellcheck disable=SC2086
  env ${vars//,/ } PS2_GT4_CAM_LOG=1 timeout ${TMO:-290} build/native-gt4/ps2run games/gt4/core.elf --disc games/gt4/disc.iso \
    --memcard "$S/mv.ps2" --load-state "$S/race.state" --run "$RUN" --press 5900:start:4 --press 6000:cross:1400 \
    --frames-dir "$S/$name" --frame-every 1000 > "$S/$name.log" 2>&1 &
  pids+=($!)
done
wait "${pids[@]}"
for spec in "$@"; do
  name=${spec%%:*}
  echo "$name: $(ls "$S/$name" | tr '\n' ' ') $(grep 'race view' "$S/$name.log" | tail -1)"
done

#!/bin/bash
# Runs two GT4 retail instances from given states at the same time and prints the last picture of each.
#   quest/tools/lan_pair.sh <stateA> <cardsA dir> <stateB> <cardsB dir> [frames]
# cards dir holds a.ps2 (slot 1) and b.ps2 (slot 2). Presses relative to each state: PAIR_PRESSES for both
# (default "+10:cross:4"), PRESS_A / PRESS_B extra per player; PAIR_SAVE=name saves <state>-name.state at the end.
set -uo pipefail
cd "$(dirname "$0")/../.."
SA=$1; DA=$2; SB=$3; DB=$4; N=${5:-2400}
SERVER=${PS2_NET_SERVER:-127.0.0.1:28312}
presses() {  # base "+rel:button:hold ..."
  local base=$1; shift
  for p in $*; do off=${p%%:*}; echo "--press $((base + ${off#+})):${p#*:}"; done
}
one() {  # state dir name [extra presses]
  local st=$1 d=$2 name=$3 f; f=$(cat "$st.frame")
  rm -rf "$d/pair"; mkdir -p "$d/pair"
  PS2_NET_SERVER=$SERVER PS2_NET_NAME=$name PS2_FRAMES_FROM=$([ -n "${PAIR_EVERY:-}" ] && echo "$f" || echo $((f + N - 4))) timeout 1500 nice build/native-gt4r/ps2run \
    games/gt4r/core.elf --disc games/gt4r/disc.iso --memcard "$d/a.ps2" --memcard2 "$d/b.ps2" --load-state "$st" \
    --run "$N" $(presses "$f" ${PAIR_PRESSES:-+10:cross:4} ${4:-}) ${PAIR_ARGS:-} \
    ${PAIR_SAVE:+--save-state $((f + N - 2)):${st%.state}-$PAIR_SAVE.state} --frames-dir "$d/pair" --frame-every ${PAIR_EVERY:-1} > "$d/pair.log" 2>&1
  local last; last=$(ls "$d/pair" | tail -1)
  ffmpeg -v error -y -i "$d/pair/$last" -vf scale=640:448 "$d/pair.png"
  [ -n "${PAIR_SAVE:-}" ] && echo $((f + N - 2)) > "${st%.state}-$PAIR_SAVE.state.frame"
}
one "$SA" "$DA" pcA "${PRESS_A:-}" &
one "$SB" "$DB" pcB "${PRESS_B:-}"
wait
ffmpeg -v error -y -i "$DA/pair.png" -i "$DB/pair.png" -filter_complex hstack=2 "$DA/../pair.png"
echo "$DA/../pair.png"
for d in "$DA" "$DB"; do grep -a "\[net\] frames" "$d/pair.log" | tail -1; done

#!/bin/bash
# GT4 retail LAN test on the PC: boots two players from scratch to the Multi-LAN "Connect" button
# (A = host for 2 players, B = client), saving a state there; then `run` connects both at once.
#   quest/tools/lan_duo.sh prepare     # ~5 min, needs $NAV_DIR/a-saved.ps2 (GT4 save, name entered)
#                                      # and $NAV_DIR/b-netcnf.ps2 (slot 2 card with network config "b")
#   quest/tools/lan_duo.sh run [frames] [extra presses for A] [extra presses for B]
# Env: NAV_DIR (default ~/.hermes/cache/scratch/netr), PS2_NET_SERVER (default 127.0.0.1:28312, a local
# wipeout-adhoc-server 28312). Frame numbers are absolute; they were found step by step with net_nav.sh.
set -uo pipefail
cd "$(dirname "$0")/../.."
D=${NAV_DIR:-$HOME/.hermes/cache/scratch/netr}
SERVER=${PS2_NET_SERVER:-127.0.0.1:28312}
RUN=build/native-gt4r/ps2run
GAME_ARGS=(games/gt4r/core.elf --disc games/gt4r/disc.iso)

boot_presses=(--press 900:start:4 --press 1500:cross:4 --press 2000:cross:4 --press 2700:triangle:4)
presses() {  # base "+rel:button:hold ..."
  local base=$1; shift
  for p in $*; do off=${p%%:*}; echo "--press $((base + ${off#+})):${p#*:}"; done
}

prepare() {
  for who in A B; do mkdir -p "$D/$who/prep"; cp "$D/a-saved.ps2" "$D/$who/a.ps2"; cp "$D/b-netcnf.ps2" "$D/$who/b.ps2"; done
  # A: Arcade menu -> title -> GT Mode -> Home -> Options -> LAN Settings: Host Yes, 2 players, OK ->
  # leave GT Mode -> Arcade -> Multi-LAN -> slot 2 -> config "b" -> Connect button.
  local a=($(presses 2990 +10:triangle:4 +500:down:4 +550:cross:4 +1450:cross:4 +2150:right:4 +2200:down:4 +2260:cross:4 \
                          +2860:down:4 +2910:right:4 +2960:right:4 +3020:cross:4)
           $(presses 6308 +10:down:4 +60:cross:4 +110:up:4 +160:cross:4 +220:down:4 +270:down:4 +320:right:4 +410:up:4 \
                          +460:cross:4 +510:right:4 +600:cross:4 +650:down:4 +700:down:4 +750:down:4 +800:down:4 +850:down:4 +900:right:4)
           $(presses 7306 +10:right:4 +60:cross:4 +410:triangle:4 +500:cross:4 +1000:triangle:4 +1210:triangle:4 +1500:cross:4)
           $(presses 9504 +10:up:4 +60:cross:4 +500:down:4 +540:right:4 +590:cross:4 +1110:left:4 +1160:cross:4 +1300:cross:4 \
                          +1360:down:4 +1410:cross:4 +1610:down:4 +1660:cross:4 +1750:down:4 +1800:down:4))
  # B: Arcade menu -> Multi-LAN -> slot 2 -> config "b" -> Connect button.
  local b=($(presses 2990 +10:down:4 +40:right:4 +80:cross:4)
           $(presses 3588 +10:left:4 +60:cross:4 +200:cross:4 +260:down:4 +310:cross:4)
           $(presses 4086 +10:down:4 +60:cross:4 +150:down:4 +200:down:4))
  PS2_NET_SERVER=$SERVER PS2_NET_NAME=pcA PS2_FRAMES_FROM=11400 timeout 900 nice $RUN "${GAME_ARGS[@]}" \
    --memcard "$D/A/a.ps2" --memcard2 "$D/A/b.ps2" --run 11404 "${boot_presses[@]}" "${a[@]}" \
    --save-state 11400:"$D/A/connect.state" --frames-dir "$D/A/prep" --frame-every 1 > "$D/A/prepare.log" 2>&1 &
  PS2_NET_SERVER=$SERVER PS2_NET_NAME=pcB PS2_FRAMES_FROM=4380 timeout 900 nice $RUN "${GAME_ARGS[@]}" \
    --memcard "$D/B/a.ps2" --memcard2 "$D/B/b.ps2" --run 4384 "${boot_presses[@]}" "${b[@]}" \
    --save-state 4380:"$D/B/connect.state" --frames-dir "$D/B/prep" --frame-every 1 > "$D/B/prepare.log" 2>&1
  wait
  for who in A B; do
    f=$(ls "$D/$who/prep" | tail -1)
    ffmpeg -v error -y -i "$D/$who/prep/$f" -vf scale=640:448 "$D/$who/connect.png"
    echo "$who: $D/$who/connect.png ($(grep -a 'state\]' "$D/$who/prepare.log" | tail -1))"
  done
}

run() {
  local frames=${1:-3000}
  for who in A B; do rm -rf "$D/$who/run"; mkdir -p "$D/$who/run"; done
  PS2_NET_SERVER=$SERVER PS2_NET_NAME=pcA PS2_FRAMES_FROM=$((11400 + frames - 4)) timeout 1800 nice $RUN "${GAME_ARGS[@]}" \
    --memcard "$D/A/a.ps2" --memcard2 "$D/A/b.ps2" --load-state "$D/A/connect.state" --run "$frames" \
    $(presses 11400 +10:cross:4 ${2:-}) ${DUO_ARGS_A:-} --frames-dir "$D/A/run" --frame-every 1 > "$D/A/run.log" 2>&1 &
  PS2_NET_SERVER=$SERVER PS2_NET_NAME=pcB PS2_FRAMES_FROM=$((4380 + frames - 4)) timeout 1800 nice $RUN "${GAME_ARGS[@]}" \
    --memcard "$D/B/a.ps2" --memcard2 "$D/B/b.ps2" --load-state "$D/B/connect.state" --run "$frames" \
    $(presses 4380 +10:cross:4 ${3:-}) --frames-dir "$D/B/run" --frame-every 1 > "$D/B/run.log" 2>&1
  wait
  for who in A B; do f=$(ls "$D/$who/run" | tail -1); ffmpeg -v error -y -i "$D/$who/run/$f" -vf scale=640:448 "$D/$who/run.png"; done
  ffmpeg -v error -y -i "$D/A/run.png" -i "$D/B/run.png" -filter_complex hstack=2 "$D/duo.png"
  echo "$D/duo.png"
  for who in A B; do echo "== $who"; grep -a "\[net\] frames" "$D/$who/run.log" | tail -1; done
}

case "${1:-}" in
  prepare) prepare ;;
  run) shift; run "$@" ;;
  *) echo "usage: $0 prepare | run [frames] [presses A] [presses B]" >&2; exit 2 ;;
esac

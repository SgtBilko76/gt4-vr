#!/bin/bash
# Race-room test on the PC: three GT4 Online copies at "Waiting for connection" against a local relay
# and a local gt4-results. A creates a race (PS2_ROOM=create -> room RpcA), B joins it, C stays in the
# open lobby. Pass: A and B receive each other's frames, C receives none of theirs, api/rooms lists RpcA
# with A as host and 2 players.
#   quest/tools/room_test.sh [frames]   (results server on 127.0.0.1:${RESULTS_PORT:-28317} must run)
set -uo pipefail
cd "$(dirname "$0")/../.."
N=${1:-900}
R=$HOME/.hermes/cache/scratch/rooms3
S=$HOME/.hermes/cache/scratch/netr
rm -rf "$R"; mkdir -p "$R"/{A,B,C}
cp "$S/A/a.ps2" "$S/A/b.ps2" "$R/A/"; cp "$S/B/a.ps2" "$S/B/b.ps2" "$R/B/"; cp "$S/A/a.ps2" "$S/A/b.ps2" "$R/C/"
( cd /mnt/Workspace/wipeout-server && exec ./wipeout-adhoc-server 28312 ) > "$R/relay.log" 2>&1 &
RELAY=$!
sleep 1
run() {  # name state dir room
  local f; f=$(cat "$2.frame")
  PS2_NET_SERVER=127.0.0.1:28312 PS2_NET_NAME=$1 PS2_RESULTS_SERVER=127.0.0.1:${RESULTS_PORT:-28317} PS2_ROOM=$4 \
    nice timeout 1200 build/native-gt4r/ps2run games/gt4r/core.elf --disc games/gt4r/disc.iso --memcard "$3/a.ps2" \
    --memcard2 "$3/b.ps2" --load-state "$2" --run "$N" --press $((f + 10)):cross:4 > "$3/run.log" 2>&1
}
run pcA "$S/A/connect.state" "$R/A" create &
run pcB "$S/B/connect.state" "$R/B" RpcA &
run pcC "$S/A/connect.state" "$R/C" "" &
sleep 60
echo "== api/rooms after 60 s"; curl -s "http://127.0.0.1:${RESULTS_PORT:-28317}/api/rooms"; echo
wait %2 %3 %4 2>/dev/null; wait
kill $RELAY 2>/dev/null
echo "== relay"; grep -a -i "join\|group\|login" "$R/relay.log" | tail -12
for x in A B C; do echo "== $x"; grep -a "\[net\] connected\|switching\|\[results\] room\|\[net\] frames" "$R/$x/run.log" | tail -4; done

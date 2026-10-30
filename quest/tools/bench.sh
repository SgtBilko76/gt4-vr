#!/bin/bash
# Race benchmark on the Quest without the user: loads the race save state (Nurburgring start, made by
# the first GT4 port; /sdcard/GT4/race.state), holds the throttle, and prints the GS thread's time per
# field, fields/s (skipped fields), the game's fps and VrApi GPU%.
#
# usage: quest/tools/bench.sh <name> [secs] [KEY=VALUE ...]     (KEY=VALUE lines go into config.txt)
#   e.g. quest/tools/bench.sh s3 70 RRV_PCSX2_GS_SCALE=3
# The user's config.txt is kept as config.user and put back afterwards, the headset settings too.
set -uo pipefail
name=${1:?name}; shift
secs=60
if [[ "${1:-}" =~ ^[0-9]+$ ]]; then secs=$1; shift; fi
adb="timeout 30 $HOME/workspace/android-sdk/platform-tools/adb -s ${QUEST:-192.168.178.115:5555}"
P=online.portroyale.gt4vr
D=/sdcard/GT4VR
out=$HOME/.hermes/cache/scratch/bench-$name
mkdir -p "$out"

$adb shell "[ -f $D/config.user ] || cp $D/config.txt $D/config.user 2>/dev/null; true"
{
  echo "load_state=/sdcard/GT4/race.state"
  echo "PS2_VU_RECORD=$D/vu1"
  for kv in "$@"; do echo "$kv"; done
} > "$out/config.txt"
$adb push "$out/config.txt" $D/config.txt > /dev/null
$adb shell "setprop debug.oculus.guardian_pause 1; svc power stayon true; am broadcast -a com.oculus.vrpowermanager.prox_close > /dev/null; setprop debug.gt4vr.padhold 0"
$adb shell am force-stop $P
sleep 2
$adb logcat -c
$adb shell monkey -p $P -c android.intent.category.LAUNCHER 1 > /dev/null
sleep 6
$adb shell setprop debug.gt4vr.padhold 16384   # Cross = throttle
sleep "$secs"
$adb shell "cat $D/log.txt" > "$out/log.txt"
$adb logcat -d | grep "VrApi.*FPS" > "$out/vrapi.txt"
$adb shell "setprop debug.gt4vr.padhold 0; am force-stop $P"
$adb shell "if [ -f $D/config.user ]; then mv $D/config.user $D/config.txt; else rm -f $D/config.txt; fi"
$adb shell "setprop debug.oculus.guardian_pause 0; svc power stayon false; am broadcast -a com.oculus.vrpowermanager.automation_disable > /dev/null"

python3 - "$out" "$name" <<'EOF'
import re, statistics as st, sys
out, name = sys.argv[1], sys.argv[2]
log = open(f"{out}/log.txt", errors="replace").read().splitlines()
loaded = any("[state] loaded" in l for l in log)
restored = any("GS memory restored" in l for l in log)
cpu = [float(m[1]) for l in log if (m := re.match(r"\[gs-wait\].*? cpu=([\d.]+)ms/f", l))]
hitch = [(float(m[1]), int(m[2])) for l in log if (m := re.match(r"\[hitch\] 10 s: \d+ fields \(([\d.]+)/s, (\d+) skipped", l))]
game = [float(m[1]) for l in log if (m := re.match(r"\[perf\] game ([\d.]+) fps", l))]
gpu = [float(m[1]) for l in open(f"{out}/vrapi.txt") if (m := re.search(r"GPU%=([\d.]+)", l))]
mhz = [int(m[1]) for l in open(f"{out}/vrapi.txt") if (m := re.search(r"CPU4/GPU=\d+/\d+,\d+/(\d+)MHz", l))]
scale = [l for l in log if "GT4 profile" in l]
half = lambda xs: xs[len(xs) // 2:] or xs   # second half = the race running
print(f"== {name}: state loaded {loaded}, GS memory {restored}; {scale[0].split(': ',1)[1][:40] if scale else 'no GT4 profile line'}")
if cpu: print(f"GS thread: {st.median(half(cpu)):.1f} ms/field median, max {max(half(cpu)):.1f}")
if hitch: print("fields/s, skipped per 10 s: " + " ".join(f"{f:.0f}/{s}" for f, s in hitch[-5:]))
if game: print("game fps: " + " ".join(f"{g:.0f}" for g in game[-8:]))
if gpu: print(f"VrApi GPU% median {st.median(half(gpu)):.2f}, max {max(half(gpu)):.2f}; GPU MHz median {st.median(half(mhz)) if mhz else '?'}")
EOF

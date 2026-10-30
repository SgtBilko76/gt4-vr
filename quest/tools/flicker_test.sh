#!/usr/bin/env bash
# Headless flicker check on the Quest: start the race state with extra config lines, take N quick
# screenshots of the curved screen (car standing), save as $S/<name>_<i>.jpg.
#   quest/tools/flicker_test.sh name "KEY=VALUE\nKEY=VALUE" [shots]
set -u
A0=$HOME/workspace/android-sdk/platform-tools/adb; A="timeout 30 $A0 -s 192.168.178.115:5555"
P=online.portroyale.gt4vr; D=/sdcard/GT4VR; S=${SCRATCH:-$HOME/.hermes/cache/scratch}
name=$1 extra=$2 shots=${3:-3}
printf "load_state=/sdcard/GT4/race.state\n$extra\n" > "$S/cfg_$name.txt"
$A push "$S/cfg_$name.txt" $D/config.txt >/dev/null
$A shell "am force-stop $P; setprop debug.gt4vr.padhold 0; am broadcast -a com.oculus.vrpowermanager.prox_close >/dev/null"
sleep 2; $A shell monkey -p $P -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1; sleep 22
for i in $(seq 1 "$shots"); do
  for try in 1 2 3; do
    $A shell "am broadcast -a com.oculus.vrpowermanager.prox_close >/dev/null"
    $A shell am startservice -n com.oculus.metacam/.capture.CaptureService -a TAKE_SCREENSHOT >/dev/null; sleep 3
    f=$($A shell ls -t /sdcard/Oculus/Screenshots/ | head -1 | tr -d '\r')
    $A pull "/sdcard/Oculus/Screenshots/$f" "$S/${name}_$i.jpg" >/dev/null 2>&1; $A shell rm "/sdcard/Oculus/Screenshots/$f"
    sz=$(stat -c %s "$S/${name}_$i.jpg" 2>/dev/null || echo 0); [ "$sz" -gt 20000 ] && break
  done
done
ffmpeg -loglevel error -y $(for i in $(seq 1 "$shots"); do echo -i "$S/${name}_$i.jpg"; done) \
  -filter_complex "$(for i in $(seq 0 $((shots-1))); do printf '[%d]scale=600:-1[v%d];' $i $i; done)$(for i in $(seq 0 $((shots-1))); do printf '[v%d]' $i; done)hstack=$shots" "$S/${name}_all.jpg"
echo "$name: $S/${name}_all.jpg"

#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Ridge Racer V: carry the save games of the Fukami RR5 app (com.bilko.rr5vr) over to this app.
# The Fukami app keeps its memory card as plain folders (/sdcard/RidgeRacerV/mc/mc0/<save>/<files>,
# the guest paths 1:1); this app uses a real 8 MB PS2 card image with ECC (/sdcard/RR5VR/memcard.ps2).
# The script pulls the folders, writes them into a card image with mymcplus and pushes it back. An
# existing card on the headset is saved first (memcard.ps2.bak-<date>) and its other saves are kept;
# a save folder that is on both is replaced by the Fukami one.
#
# usage: quest/tools/rr5_import_fukami_saves.sh [adb serial]   (both apps must be closed)
#        LOCAL_ONLY=<dir with mc0> OUT=<card.ps2> ...          (PC test: no adb)
set -euo pipefail
ADB=${ADB:-/mnt/Workspace/android-sdk/platform-tools/adb}
SERIAL=${1:-192.168.178.115:5555}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/rr5mc.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
mymc() { uv run -q --with mymcplus mymcplus "$@"; }

if [[ -n "${LOCAL_ONLY:-}" ]]; then
  cp -r "$LOCAL_ONLY/mc0" "$WORK/mc0"
  CARD=${OUT:?OUT=<card.ps2> needed with LOCAL_ONLY}
else
  A=("$ADB" -s "$SERIAL")
  "${A[@]}" shell 'ls /sdcard/RidgeRacerV/mc/mc0' >/dev/null || { echo "no Fukami saves (/sdcard/RidgeRacerV/mc/mc0)" >&2; exit 1; }
  "${A[@]}" pull /sdcard/RidgeRacerV/mc/mc0 "$WORK/" >/dev/null
  CARD=$WORK/memcard.ps2
  if "${A[@]}" shell 'test -f /sdcard/RR5VR/memcard.ps2'; then
    "${A[@]}" pull /sdcard/RR5VR/memcard.ps2 "$CARD" >/dev/null
    stamp=$(date +%Y%m%d-%H%M%S)
    "${A[@]}" shell "cp /sdcard/RR5VR/memcard.ps2 /sdcard/RR5VR/memcard.ps2.bak-$stamp"
    echo "existing card kept as /sdcard/RR5VR/memcard.ps2.bak-$stamp"
  fi
fi

shopt -s nullglob
# Always a freshly formatted card (deleting a save in place left lost clusters behind): the old
# card's other saves are exported (.psu) and imported into the new one.
mkdir -p "$WORK/keep"
if [[ -s "$CARD" ]]; then
  for save in $(mymc "$CARD" ls / | awk '$1 ~ /d/ && $NF != "." && $NF != ".." {print $NF}'); do
    if [[ -d "$WORK/mc0/$save" ]]; then echo "save $save: replaced by the Fukami app's"; continue; fi
    (cd "$WORK/keep" && mymc "$CARD" export "$save" >/dev/null) && echo "save $save: kept"
  done
fi
rm -f "$CARD"
mymc "$CARD" format
for psu in "$WORK"/keep/*.psu; do mymc "$CARD" import "$psu" >/dev/null; done
n=0
for dir in "$WORK"/mc0/*/; do
  name=$(basename "$dir")
  files=("$dir"*)
  [[ ${#files[@]} -gt 0 ]] || continue
  mymc "$CARD" mkdir "$name"
  mymc "$CARD" add -d "$name" "${files[@]}"
  echo "save $name: ${#files[@]} files"
  n=$((n + 1))
done
[[ $n -gt 0 ]] || { echo "no save folders found" >&2; exit 1; }
mymc "$CARD" check
mymc "$CARD" dir

if [[ -z "${LOCAL_ONLY:-}" ]]; then
  "${A[@]}" push "$CARD" /sdcard/RR5VR/memcard.ps2 >/dev/null
  echo "card pushed to /sdcard/RR5VR/memcard.ps2 ($n saves from the Fukami app)"
fi

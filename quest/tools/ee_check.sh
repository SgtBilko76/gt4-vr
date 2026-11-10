#!/system/bin/sh
# Correctness on the headset: race pictures of the JIT vs the interpreter (software drawing on).
cd /data/local/tmp/ee
FRAMES=${FRAMES:-240}
for m in jit interp; do
    rm -rf fr-$m; mkdir -p fr-$m
    cp memcard.ps2 mc-c$m.ps2
    if [ $m = jit ]; then J=1; else J=0; fi
    env PS2_EE_INTERP=1 PS2_EE_JIT=$J ./ps2run core.elf --disc disc.iso --memcard mc-c$m.ps2 --load-state race.state \
        --run $FRAMES --press 5890:cross:$FRAMES --frames-dir fr-$m --frame-every 40 > clog-$m.txt 2>&1
done
same=0; diff=0
for f in $(ls fr-interp); do
    if cmp -s fr-interp/$f fr-jit/$f; then same=$((same+1)); else diff=$((diff+1)); echo "DIFF $f"; fi
done
echo "pictures: $same same, $diff different"

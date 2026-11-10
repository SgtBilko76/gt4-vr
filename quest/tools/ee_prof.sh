#!/system/bin/sh
# Profile the EE modes with simpleperf (300 race frames each), print the top functions.
cd /data/local/tmp/ee
chmod 755 ps2run-sym
for m in ${PMODES:-recomp jit}; do
    cp memcard.ps2 mc-p$m.ps2
    if [ $m = jit ]; then E="PS2_EE_INTERP=1"; else E="PS2_EE_X=0"; fi
    env PS2_GS_NULL=1 $E simpleperf record -o perf-$m.data -f 2000 ./ps2run-sym core.elf --disc disc.iso \
        --memcard mc-p$m.ps2 --load-state race.state --run 300 --press 5890:cross:300 > plog-$m.txt 2>&1
    echo "===== $m: by dso"
    simpleperf report -i perf-$m.data --sort dso 2>/dev/null | sed -n '/Overhead/,$p' | head -8
    echo "===== $m: top symbols (self)"
    simpleperf report -i perf-$m.data --sort dso,symbol --children 2>/dev/null >/dev/null
    simpleperf report -i perf-$m.data --sort symbol 2>/dev/null | sed -n '/Overhead/,$p' | head -30
done

#!/system/bin/sh
# EE speed on the headset: same race state, 600 frames, gas held, no software drawing.
# modes: recomp (generated code), interp (PS2_EE_INTERP=1 PS2_EE_JIT=0), jit (PS2_EE_INTERP=1)
#        nogen / nogen-vuint: ps2run-nogen (no game code: EE JIT + VU1 ARM64 JIT / VU interpreter),
#        pushed from build/arm64-interp/ps2run
cd /data/local/tmp/ee
FRAMES=${FRAMES:-600}
run() {
    name=$1; shift
    bin=./ps2run
    case $name in nogen*) bin=./ps2run-nogen ;; esac
    cp memcard.ps2 mc-$name.ps2
    t0=$(date +%s.%N)  # mksh arithmetic is 32-bit: subtract in awk
    env PS2_GS_NULL=1 "$@" $bin core.elf --disc disc.iso --memcard mc-$name.ps2 --load-state race.state \
        --run $FRAMES --press 5890:cross:$FRAMES > log-$name.txt 2>&1
    rc=$?
    t1=$(date +%s.%N)
    awk -v a=$t0 -v b=$t1 -v f=$FRAMES -v n=$name -v rc=$rc 'BEGIN { s = b - a; printf "%s rc=%d %.1f s for %d frames = %.2f ms/frame (%.1f fps)\n", n, rc, s, f, s * 1000 / f, f / s }'
    grep -h 'stopped after\|stopped at\|unsupported\|JIT\|not compiled' log-$name.txt | sort | uniq -c | head -8 | sed 's/^/    /' 
}
for m in ${MODES:-recomp jit interp}; do
    case $m in
    recomp) run recomp ;;
    jit) run jit PS2_EE_INTERP=1 ;;
    interp) run interp PS2_EE_INTERP=1 PS2_EE_JIT=0 ;;
    nogen) run nogen PS2_VU_JIT=1 ;;
    nogen-vuint) run nogen-vuint PS2_VU_JIT=0 ;;
    esac
done

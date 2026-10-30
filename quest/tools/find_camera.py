#!/usr/bin/env python3
"""Find GT4's CameraBase::getModelviewMatrix / getProjectionMatrix in a GT4-family ELF.

Both are tiny wrappers around the pgl matrix stack (names and shape from gt4-decomp, SCUS-97328
0x44F210 / 0x44F280):
    jal push        (a function that is just `vcallms 0x900`)
    jal loadIdentity
    <virtual call: applyProjection / applyModelview>
    jal getMatrix(target)
    jal pop         (a function ending in `vcallms 0x930`)
This finds `push` and `pop` by their VU0 microprogram addresses and lists every function calling
both, with the calls in between.

usage: find_camera.py <elf>
Run with: uv run --with capstone quest/tools/find_camera.py games/gt4/core.elf
"""
import struct
import sys

import capstone

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from ps2dis import load_segments  # noqa: E402

VCALLMS = lambda addr: 0x4A000038 | ((addr // 8) << 6)  # COP2 VCALLMS imm15 = addr / 8
JR_RA = 0x03E00008


def words(segs):
    for vaddr, b in segs:
        for i in range(0, len(b) - 3, 4):
            yield vaddr + i, struct.unpack_from("<I", b, i)[0]


def main():
    segs = load_segments(sys.argv[1])
    code = dict(words(segs))
    push = [a for a, w in code.items() if w == VCALLMS(0x900) and code.get(a - 4) == JR_RA or
            (w == VCALLMS(0x900) and code.get(a + 4) == JR_RA)]
    pop = [a for a, w in code.items() if w == VCALLMS(0x930)]
    # function start: the word right after the previous `jr ra` + delay slot
    def start_of(a):
        x = a
        while x - 8 in code and code[x - 8] != JR_RA:
            x -= 4
        return x
    push_fns = sorted({start_of(a) for a in push})
    pop_fns = sorted({start_of(a) for a in pop})
    print("push candidates:", [hex(x) for x in push_fns])
    print("pop candidates:", [hex(x) for x in pop_fns])
    jal = lambda target: 0x0C000000 | ((target >> 2) & 0x03FFFFFF)
    md = capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS64 + capstone.CS_MODE_LITTLE_ENDIAN)
    for p in push_fns:
        callers = [a for a, w in code.items() if w == jal(p)]
        for c in callers:
            f = start_of(c)
            # a wrapper: within 0x80 bytes of its start it also calls a pop function
            body = [(a, code[a]) for a in range(f, f + 0x80, 4) if a in code]
            if any(w == jal(q) for _, w in body for q in pop_fns):
                blob = b"".join(struct.pack("<I", w) for _, w in body)
                print(f"\n== wrapper at {f:#x} (calls push {p:#x})")
                for ins in md.disasm(blob, f):
                    print(f"  {ins.address:08x}  {ins.mnemonic} {ins.op_str}")
                    if ins.mnemonic == "jr" and ins.op_str == "$ra":
                        break


if __name__ == "__main__":
    main()

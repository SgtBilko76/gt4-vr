#!/usr/bin/env python3
"""Find the vtable of a g++ 2.96 class in a PS2 ELF by its RTTI name, and list its slots.

g++ 2.96 (old ABI, no thunks): the class's __tf function loads the mangled name with lui/addiu;
the vtable holds 8-byte entries {short delta, short index, fn}, the first entry's fn being __tf.

usage: vtable.py <elf> <mangled name, e.g. 10CameraBase> [slots]
"""
import struct
import sys


def load(path):
    data = open(path, "rb").read()
    phoff, = struct.unpack_from("<I", data, 28)
    phentsize, phnum = struct.unpack_from("<HH", data, 42)
    segs = []
    for i in range(phnum):
        t, off, va, _, fs, ms, fl, _ = struct.unpack_from("<8I", data, phoff + i * phentsize)
        if t == 1:
            segs.append((va, data[off:off + fs]))
    return segs


def words(segs):
    for va, b in segs:
        for j in range(0, len(b) - 3, 4):
            yield va + j, struct.unpack_from("<I", b, j)[0]


def find_bytes(segs, needle):
    for va, b in segs:
        i = b.find(needle)
        while i >= 0:
            yield va + i
            i = b.find(needle, i + 1)


def lui_addiu_refs(code, target):
    """Code addresses that form `target` with lui rX, hi ; ... addiu/ori rY, rX, lo (within 16 words)."""
    hi = ((target + 0x8000) >> 16) & 0xFFFF
    lo = (target - (hi << 16)) & 0xFFFF
    out = []
    for a, w in code.items():
        if (w >> 26) == 0x0F and (w & 0xFFFF) == hi:
            rt = (w >> 16) & 31
            for k in range(1, 17):
                w2 = code.get(a + 4 * k)
                if w2 is None:
                    break
                if (w2 >> 26) in (0x09, 0x0D) and (w2 >> 21) & 31 == rt and (w2 & 0xFFFF) == lo:
                    out.append(a)
                    break
    return out


def func_start(code, a):
    while code.get(a - 8) != 0x03E00008 and a - 4 in code:
        a -= 4
    while code.get(a) == 0:  # alignment padding after the previous function
        a += 4
    return a


def main():
    segs = load(sys.argv[1])
    name = sys.argv[2].encode() + b"\0"
    slots = int(sys.argv[3]) if len(sys.argv) > 3 else 12
    code = dict(words(segs))
    for s in find_bytes(segs, name):
        if s > 0 and any(s - 1 >= va and s - 1 < va + len(b) and b[s - 1 - va] != 0 for va, b in segs):
            continue  # inside a longer string
        print(f"name {sys.argv[2]} at {s:#x}")
        for ref in lui_addiu_refs(code, s):
            tf = func_start(code, ref)
            vt = [a for a, w in code.items() if w == tf]
            print(f"  __tf {tf:#x} (ref {ref:#x}), pointed to from {[hex(v) for v in vt]}")
            for v in vt:
                base = v - 4  # entry = {delta, index} word, then fn
                print(f"  vtable at {base:#x}:")
                for i in range(slots):
                    d = code.get(base + 8 * i, 0)
                    fn = code.get(base + 8 * i + 4, 0)
                    print(f"    slot {i:2}: delta {d & 0xFFFF:#06x} index {d >> 16:#06x} fn {fn:#x}")


if __name__ == "__main__":
    main()

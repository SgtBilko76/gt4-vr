#!/usr/bin/env python3
"""Inspect EE RAM in a ps2run state file (GT4 Online lap-result research).

usage: stateram.py STATE [cmd args...]
  find TEXT...        print RAM offsets of each string and who points at them
  dump ADDR [LEN]     hex dump
  spec                print the RaceSpec reached through the static pointer at 0x622F4C
"""
import re
import struct
import sys

ELF = "/mnt/Workspace/GT4-VR/games/gt4r/core.elf"
RAM_SIZE = 32 * 1024 * 1024


def load_ram(path):
    elf = open(ELF, "rb").read()
    st = open(path, "rb").read()
    phoff = struct.unpack_from("<I", elf, 0x1C)[0]
    for i in range(struct.unpack_from("<H", elf, 0x2C)[0]):
        t, off, va, _pa, fs, *_ = struct.unpack_from("<8I", elf, phoff + i * 32)
        if t == 1 and fs > 0x2000:
            k = st.find(elf[off + 0x1000:off + 0x1040])
            base = k - (va + 0x1000)
            return st[base:base + RAM_SIZE]
    raise SystemExit("no text segment")


def u32(ram, a):
    return struct.unpack_from("<I", ram, a & (RAM_SIZE - 1))[0]


def cstr(ram, a, n=64):
    a &= RAM_SIZE - 1
    return ram[a:a + n].split(b"\0")[0].decode("latin-1")


def refs(ram, v, lim=8):
    p = struct.pack("<I", v)
    return [m.start() for m in re.finditer(re.escape(p), ram)][:lim]


def dump(ram, a, n=0x100):
    for o in range(a, a + n, 16):
        b = ram[o:o + 16]
        print(f"{o:08x} {b.hex(' ')}  {''.join(chr(c) if 32 <= c < 127 else '.' for c in b)}")


def main():
    ram = load_ram(sys.argv[1])
    cmd = sys.argv[2] if len(sys.argv) > 2 else "spec"
    if cmd == "find":
        for w in sys.argv[3:]:
            for h in [m.start() for m in re.finditer(re.escape(w.encode()), ram)][:12]:
                print(f"{w!r} @ {h:08x} <- {[hex(r) for r in refs(ram, h)]}")
    elif cmd == "dump":
        dump(ram, int(sys.argv[3], 16), int(sys.argv[4], 16) if len(sys.argv) > 4 else 0x100)
    elif cmd == "spec":
        p = u32(ram, 0x622F4C)
        print(f"[0x622F4C] = {p:08x} vtable {u32(ram, p):08x}")
        print("course", cstr(ram, p + 4), "| race", cstr(ram, p + 0x44))
        n = u32(ram, p + 0xC4)
        print("entries", n)
        for i in range(min(n, 16)):
            e = p + 0xC8 + i * 0x68
            print(i, [u32(ram, e + k) for k in range(0, 0x18, 4)], cstr(ram, e + 0x18))
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()

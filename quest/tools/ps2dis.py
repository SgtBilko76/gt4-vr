#!/usr/bin/env python3
"""Disassemble a range of a PS2 ELF (by virtual address), last segment wins.

usage: ps2dis.py <elf> <start_hex> <count_words>
Run with: uv run --with capstone quest/tools/ps2dis.py ...
"""
import struct
import sys

import capstone


def load_segments(path):
    data = open(path, "rb").read()
    phoff, = struct.unpack_from("<I", data, 28)
    phentsize, phnum = struct.unpack_from("<HH", data, 42)
    segs = []
    for i in range(phnum):
        p_type, off, vaddr, _, filesz, memsz, flags, _ = struct.unpack_from("<8I", data, phoff + i * phentsize)
        if p_type == 1:
            segs.append((vaddr, data[off:off + filesz] + b"\0" * (memsz - filesz)))
    return segs


def read(segs, addr, n):
    for vaddr, b in reversed(segs):
        if vaddr <= addr and addr + n <= vaddr + len(b):
            return b[addr - vaddr:addr - vaddr + n]
    raise KeyError(hex(addr))


def main():
    segs = load_segments(sys.argv[1])
    start = int(sys.argv[2], 16)
    count = int(sys.argv[3])
    md = capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS64 + capstone.CS_MODE_LITTLE_ENDIAN)
    code = read(segs, start, count * 4)
    for i in range(count):
        word = code[i * 4:i * 4 + 4]
        ins = list(md.disasm(word, start + i * 4))
        text = f"{ins[0].mnemonic} {ins[0].op_str}" if ins else f".word 0x{struct.unpack('<I', word)[0]:08x}"
        print(f"{start + i * 4:08x}: {struct.unpack('<I', word)[0]:08x}  {text}")


if __name__ == "__main__":
    main()

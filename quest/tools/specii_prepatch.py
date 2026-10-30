#!/usr/bin/env python3
"""Bake GT4 A-Spec II's boot-time code patches into the ELF.

The Spec II mod's .plugin runs once from crt0 (jal 0x68a760 -> j 0x68a480) and
rewrites game code with word/halfword/float stores. A static recompiler never
sees those stores, so this runs the routine in a small R5900 subset
interpreter, collects every store that lands in a file-backed segment, and
writes them into a copy of the ELF (games/gt4/core.elf). The routine is left
in place, so it still runs at boot (stores into .bss etc. stay correct).

Calls that leave the plugin are not executed: they return 0 and are logged.

usage: specii_prepatch.py <in.elf> <out.elf>
"""
import struct
import sys

PLUGIN_ENTRY = 0x68A480
RETURN_SENTINEL = 0xFFFFFFF0
STACK_TOP = 0x01F00000


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


def s32(v):
    v &= 0xFFFFFFFF
    return v - 0x100000000 if v & 0x80000000 else v


def u64(v):
    return v & 0xFFFFFFFFFFFFFFFF


def sext32(v):
    return u64(s32(v))


class Elf:
    def __init__(self, data):
        self.data = bytearray(data)
        phoff, = struct.unpack_from("<I", data, 28)
        phentsize, phnum = struct.unpack_from("<HH", data, 42)
        self.segments = []  # (vaddr, file offset, filesz, memsz)
        for i in range(phnum):
            p_type, off, vaddr, _, filesz, memsz, _, _ = struct.unpack_from("<8I", data, phoff + i * phentsize)
            if p_type == 1:
                self.segments.append((vaddr, off, filesz, memsz))

    def file_offset(self, addr, size):
        # last segment wins, like the loader
        for vaddr, off, filesz, _ in reversed(self.segments):
            if vaddr <= addr and addr + size <= vaddr + filesz:
                return off + addr - vaddr
        return None

    def in_memory(self, addr):
        return any(v <= addr < v + m for v, _, _, m in self.segments)


class Cpu:
    def __init__(self, elf, plugin_range):
        self.elf = elf
        self.plugin_range = plugin_range
        self.mem = {}  # byte address -> value, overlays the ELF image
        self.gpr = [0] * 32
        self.fpr = [0] * 32  # raw 32-bit
        self.hi = self.lo = 0
        self.stores = {}  # address -> byte, only file-backed, non-stack
        self.external_calls = []

    # memory
    def rb(self, a):
        a &= 0xFFFFFFFF
        if a in self.mem:
            return self.mem[a]
        off = self.elf.file_offset(a, 1)
        return self.elf.data[off] if off is not None else 0

    def wb(self, a, v):
        a &= 0xFFFFFFFF
        self.mem[a] = v & 0xFF
        if not (STACK_TOP - 0x100000 <= a < STACK_TOP) and self.elf.file_offset(a, 1) is not None:
            self.stores[a] = v & 0xFF

    def read(self, a, n):
        return int.from_bytes(bytes(self.rb(a + i) for i in range(n)), "little")

    def write(self, a, v, n):
        for i in range(n):
            self.wb(a + i, (v >> (8 * i)) & 0xFF)

    def reg(self, r):
        return self.gpr[r]

    def setreg(self, r, v):
        if r:
            self.gpr[r] = u64(v)

    def run(self, entry, max_steps=5_000_000):
        self.gpr[29] = STACK_TOP
        self.gpr[31] = RETURN_SENTINEL
        pc = entry
        delay_target = None
        steps = 0
        while True:
            steps += 1
            if steps > max_steps:
                raise RuntimeError("step limit")
            if pc == RETURN_SENTINEL:
                return steps
            word = self.read(pc, 4)
            next_pc = pc + 4
            branch = self.step(pc, word)
            if delay_target is not None:
                next_pc = delay_target
                delay_target = None
            if branch is not None:
                kind, target = branch
                if kind == "likely-skip":
                    next_pc = pc + 8
                elif kind == "call-external":
                    # execute the delay slot, then pretend the callee returned 0
                    self.step(pc + 4, self.read(pc + 4, 4))
                    self.external_calls.append((pc, target))
                    self.setreg(2, 0)
                    self.setreg(3, 0)
                    next_pc = pc + 8
                else:
                    delay_target = target
            pc = next_pc

    def call(self, pc, target, link_reg):
        self.setreg(link_reg, pc + 8)
        lo, hi = self.plugin_range
        if lo <= target < hi:
            return ("branch", target)
        return ("call-external", target)

    def step(self, pc, w):
        op = w >> 26
        rs, rt, rd = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
        sa, fn = (w >> 6) & 31, w & 63
        imm = w & 0xFFFF
        simm = s16(imm)
        R = self.reg
        if w == 0:
            return None
        if op == 0:  # SPECIAL
            if fn == 0x00: self.setreg(rd, sext32((R(rt) << sa) & 0xFFFFFFFF))
            elif fn == 0x02: self.setreg(rd, sext32((R(rt) & 0xFFFFFFFF) >> sa))
            elif fn == 0x03: self.setreg(rd, sext32(s32(R(rt)) >> sa))
            elif fn == 0x04: self.setreg(rd, sext32((R(rt) << (R(rs) & 31)) & 0xFFFFFFFF))
            elif fn == 0x06: self.setreg(rd, sext32((R(rt) & 0xFFFFFFFF) >> (R(rs) & 31)))
            elif fn == 0x07: self.setreg(rd, sext32(s32(R(rt)) >> (R(rs) & 31)))
            elif fn == 0x08: return ("branch", R(rs) & 0xFFFFFFFF)  # jr
            elif fn == 0x09: return self.call(pc, R(rs) & 0xFFFFFFFF, rd)  # jalr
            elif fn == 0x0A:
                if R(rt) == 0: self.setreg(rd, R(rs))
            elif fn == 0x0B:
                if R(rt) != 0: self.setreg(rd, R(rs))
            elif fn == 0x0F: pass  # sync
            elif fn == 0x10: self.setreg(rd, self.hi)
            elif fn == 0x12: self.setreg(rd, self.lo)
            elif fn == 0x18:
                p = s32(R(rs)) * s32(R(rt)); self.lo = sext32(p); self.hi = sext32(p >> 32)
                if rd: self.setreg(rd, self.lo)
            elif fn == 0x19:
                p = (R(rs) & 0xFFFFFFFF) * (R(rt) & 0xFFFFFFFF); self.lo = sext32(p); self.hi = sext32(p >> 32)
                if rd: self.setreg(rd, self.lo)
            elif fn in (0x1A, 0x1B):
                a, b = (s32(R(rs)), s32(R(rt))) if fn == 0x1A else (R(rs) & 0xFFFFFFFF, R(rt) & 0xFFFFFFFF)
                if b:
                    q = abs(a) // abs(b) * (1 if (a >= 0) == (b >= 0) else -1)
                    self.lo, self.hi = sext32(q), sext32(a - q * b)
            elif fn in (0x20, 0x21): self.setreg(rd, sext32(R(rs) + R(rt)))
            elif fn in (0x22, 0x23): self.setreg(rd, sext32(R(rs) - R(rt)))
            elif fn == 0x24: self.setreg(rd, R(rs) & R(rt))
            elif fn == 0x25: self.setreg(rd, R(rs) | R(rt))
            elif fn == 0x26: self.setreg(rd, R(rs) ^ R(rt))
            elif fn == 0x27: self.setreg(rd, ~(R(rs) | R(rt)))
            elif fn == 0x2A: self.setreg(rd, int(s32(R(rs)) < s32(R(rt))) if True else 0)
            elif fn == 0x2B: self.setreg(rd, int((R(rs) & 0xFFFFFFFF) < (R(rt) & 0xFFFFFFFF)))
            elif fn in (0x2C, 0x2D): self.setreg(rd, R(rs) + R(rt))  # dadd(u)
            elif fn in (0x2E, 0x2F): self.setreg(rd, R(rs) - R(rt))
            elif fn == 0x38: self.setreg(rd, R(rt) << sa)
            elif fn == 0x3A: self.setreg(rd, R(rt) >> sa)
            elif fn == 0x3C: self.setreg(rd, R(rt) << (sa + 32))
            elif fn == 0x3E: self.setreg(rd, R(rt) >> (sa + 32))
            elif fn == 0x3F:
                v = R(rt) - (1 << 64) if R(rt) >> 63 else R(rt)
                self.setreg(rd, v >> (sa + 32))
            else:
                raise NotImplementedError(f"{pc:08x}: SPECIAL fn {fn:#x} ({w:08x})")
            return None
        if op == 1:  # REGIMM
            v = s32(R(rs)) if False else (R(rs) - (1 << 64) if R(rs) >> 63 else R(rs))
            target = pc + 4 + (simm << 2)
            if rt in (0, 2): taken = v < 0
            elif rt in (1, 3): taken = v >= 0
            else: raise NotImplementedError(f"{pc:08x}: REGIMM {rt}")
            if taken: return ("branch", target)
            return ("likely-skip", 0) if rt in (2, 3) else None
        if op in (2, 3):
            target = ((pc + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
            if op == 2: return ("branch", target)
            return self.call(pc, target, 31)
        if op in (4, 5, 6, 7, 0x14, 0x15, 0x16, 0x17):
            a = R(rs) - (1 << 64) if R(rs) >> 63 else R(rs)
            b = R(rt) - (1 << 64) if R(rt) >> 63 else R(rt)
            base = op & 0x0F if op >= 0x14 else op
            base = {0x4: 4, 0x5: 5, 0x6: 6, 0x7: 7}[base if base >= 4 else base + 4] if False else (op - 0x10 if op >= 0x14 else op)
            taken = {4: a == b, 5: a != b, 6: a <= 0, 7: a > 0}[base]
            if taken: return ("branch", pc + 4 + (simm << 2))
            return ("likely-skip", 0) if op >= 0x14 else None
        if op in (8, 9): self.setreg(rt, sext32(R(rs) + simm)); return None
        if op == 0x0A: self.setreg(rt, int(s32(R(rs)) < simm)); return None
        if op == 0x0B: self.setreg(rt, int((R(rs) & 0xFFFFFFFF) < (u64(simm) & 0xFFFFFFFF))); return None
        if op == 0x0C: self.setreg(rt, R(rs) & imm); return None
        if op == 0x0D: self.setreg(rt, R(rs) | imm); return None
        if op == 0x0E: self.setreg(rt, R(rs) ^ imm); return None
        if op == 0x0F: self.setreg(rt, sext32(imm << 16)); return None
        if op in (0x18, 0x19): self.setreg(rt, R(rs) + simm); return None  # daddi(u)
        addr = (R(rs) + simm) & 0xFFFFFFFF
        if op == 0x20: self.setreg(rt, u64(s16(self.read(addr, 1)) if False else (self.read(addr, 1) - 256 if self.read(addr, 1) & 0x80 else self.read(addr, 1)))); return None
        if op == 0x24: self.setreg(rt, self.read(addr, 1)); return None
        if op == 0x21: self.setreg(rt, u64(s16(self.read(addr, 2)))); return None
        if op == 0x25: self.setreg(rt, self.read(addr, 2)); return None
        if op == 0x23: self.setreg(rt, sext32(self.read(addr, 4))); return None
        if op == 0x27: self.setreg(rt, self.read(addr, 4)); return None
        if op == 0x37: self.setreg(rt, self.read(addr, 8)); return None
        if op == 0x28: self.write(addr, R(rt), 1); return None
        if op == 0x29: self.write(addr, R(rt), 2); return None
        if op == 0x2B: self.write(addr, R(rt), 4); return None
        if op == 0x3F: self.write(addr, R(rt), 8); return None
        if op == 0x31: self.fpr[rt] = self.read(addr, 4); return None  # lwc1
        if op == 0x39: self.write(addr, self.fpr[rt], 4); return None  # swc1
        if op == 0x2F: return None  # cache
        if op == 0x11:  # COP1 moves only
            fmt = rs
            if fmt == 0x00: self.setreg(rt, sext32(self.fpr[rd])); return None  # mfc1
            if fmt == 0x04: self.fpr[rd] = R(rt) & 0xFFFFFFFF; return None  # mtc1
            if fmt == 0x10 and fn == 0x06: self.fpr[sa] = self.fpr[rd]; return None  # mov.s
        raise NotImplementedError(f"{pc:08x}: opcode {op:#x} ({w:08x})")


def main():
    src, dst = sys.argv[1], sys.argv[2]
    elf = Elf(open(src, "rb").read())
    plugin = next(s for s in elf.segments if s[0] <= PLUGIN_ENTRY < s[0] + s[3])
    cpu = Cpu(elf, (plugin[0], plugin[0] + plugin[3]))
    steps = cpu.run(PLUGIN_ENTRY)
    print(f"ran {steps} instructions")
    for pc, target in cpu.external_calls:
        print(f"  external call at {pc:08x} -> {target:08x} (skipped, returned 0)")
    # group stores into words for the report
    words = {}
    for a, v in cpu.stores.items():
        words.setdefault(a & ~3, {})[a & 3] = v
    changed = 0
    for wa in sorted(words):
        for a, v in words[wa].items():
            off = elf.file_offset(wa + a, 1)
            if elf.data[off] != v:
                elf.data[off] = v
                changed += 1
    print(f"{len(words)} patched words, {changed} bytes changed")
    text_words = [wa for wa in sorted(words) if 0x100000 <= wa < 0x65FBBC]
    print(f"  {len(text_words)} words in .text, first: {', '.join(hex(w) for w in text_words[:8])}")
    open(dst, "wb").write(elf.data)


if __name__ == "__main__":
    main()

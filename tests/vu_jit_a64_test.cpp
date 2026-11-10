// ARM64 VU1 JIT check (run under qemu-aarch64 on the PC or on the headset): replays VU1 states saved by
// PS2_VU_JIT=check PS2_VU_SNAP=<dir> (snap_*.bin: entry, TOP, ITOP, code, data memory, registers) through
//   A = the ARM64 back end (vu_jit_a64.cpp) and
//   W = a small reference interpreter of the same WebAssembly module (Translator output, vu_jit.cpp),
//   I = optionally the VU interpreter (third argument "interp").
// A == W: the back end is right and any difference to the VU interpreter comes from the Translator.
// A != W: the back end is wrong; the first differing register/word is printed.
//   build: NDK clang++ -O2 -std=gnu++20 -static -Iruntime/include -Iruntime/src tests/vu_jit_a64_test.cpp
//          build/arm64-interp/libps2game.a build/arm64-interp/libps2runtime.a -ldl
//   run:   qemu-aarch64 vu_jit_a64_test <snap dir> [interp]
#include "hw.hpp"
#include "vu_jit.hpp"
#include "vu_jit_web.hpp"
#include "vu_micro.hpp"
#include "ps2/vu_ops.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

using namespace ps2;

namespace ps2::vu {
void set_tops(int unit, std::uint32_t top, std::uint32_t itop);
void interpret(int unit, std::uint32_t pc);
}

namespace {

struct V {
    std::uint64_t lo = 0, hi = 0;
};

struct Ins {
    std::uint8_t op = 0;
    std::uint32_t sub = 0, a = 0, b = 0;
    std::int64_t imm = 0;
    std::uint8_t bytes[16] = {};
    std::vector<std::uint32_t> targets;
    std::size_t match = 0;  // block/loop/if: index of the matching end
};

struct Rd {
    const std::uint8_t *p, *end;
    std::uint8_t byte() { return p < end ? *p++ : 0; }
    std::uint32_t u32() {
        std::uint32_t v = 0;
        for (int s = 0; s < 35; s += 7) {
            const std::uint8_t c = byte();
            v |= static_cast<std::uint32_t>(c & 0x7Fu) << s;
            if (!(c & 0x80u)) break;
        }
        return v;
    }
    std::int64_t sleb() {
        std::int64_t v = 0;
        int s = 0;
        std::uint8_t c;
        do {
            c = byte();
            v |= static_cast<std::int64_t>(c & 0x7Fu) << s;
            s += 7;
        } while (c & 0x80u);
        if (s < 64 && (c & 0x40u)) v |= -(static_cast<std::int64_t>(1) << s);
        return v;
    }
};

struct Module {
    std::vector<Ins> code;
    std::vector<int> local_kind;  // 0 i32/i64/f64 (8 bytes), 1 v128
    bool ok = false;
};

Module parse(const std::vector<std::uint8_t> &m) {
    Module mod;
    Rd r{m.data() + 8, m.data() + m.size()};
    while (r.p < r.end) {
        const std::uint8_t id = r.byte();
        const std::uint32_t size = r.u32();
        if (id != 10) {
            r.p += size;
            continue;
        }
        Rd s{r.p, r.p + size};
        s.u32();
        const std::uint32_t body = s.u32();
        Rd b{s.p, s.p + body};
        mod.local_kind = {0, 0};
        const std::uint32_t groups = b.u32();
        for (std::uint32_t g = 0; g < groups; ++g) {
            const std::uint32_t n = b.u32();
            const std::uint8_t t = b.byte();
            for (std::uint32_t k = 0; k < n; ++k) mod.local_kind.push_back(t == 0x7B ? 1 : 0);
        }
        std::vector<std::size_t> open;
        while (b.p < b.end) {
            Ins in;
            in.op = b.byte();
            switch (in.op) {
            case 0x02: case 0x03: case 0x04:
                b.byte();
                open.push_back(mod.code.size());
                break;
            case 0x0B:
                if (!open.empty()) {
                    mod.code[open.back()].match = mod.code.size();
                    open.pop_back();
                }
                break;
            case 0x0C: case 0x0D: case 0x10: case 0x12: case 0x20: case 0x21: case 0x22: in.a = b.u32(); break;
            case 0x0E: {
                const std::uint32_t n = b.u32();
                for (std::uint32_t k = 0; k <= n; ++k) in.targets.push_back(b.u32());
                break;
            }
            case 0x41: in.imm = static_cast<std::int32_t>(b.sleb()); break;
            case 0x42: in.imm = b.sleb(); break;
            case 0xFD:
                in.sub = b.u32();
                if (in.sub == 0x00 || in.sub == 0x0B) {
                    in.a = b.u32();
                    in.b = b.u32();
                } else if (in.sub == 0x0C || in.sub == 0x0D) {
                    for (auto &x : in.bytes) x = b.byte();
                } else if (in.sub >= 0x15 && in.sub <= 0x22) {
                    in.a = b.byte();
                }
                break;
            default:
                if (in.op >= 0x28 && in.op <= 0x3E) {
                    in.a = b.u32();
                    in.b = b.u32();
                }
            }
            mod.code.push_back(std::move(in));
        }
        mod.ok = true;
        return mod;
    }
    return mod;
}

using F0 = std::uint32_t (*)();
using F1 = std::uint32_t (*)(std::uint32_t);
using F2 = std::uint32_t (*)(std::uint32_t, std::uint32_t);
using F4 = std::uint32_t (*)(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);
constexpr std::uint8_t kParams[vu::jit::kImports] = {1, 1, 1, 2, 4, 1, 0, 0, 2};
constexpr bool kResult[vu::jit::kImports] = {true, true, true, false, false, false, true, true, false};

float f(std::uint32_t b) {
    float x;
    std::memcpy(&x, &b, 4);
    return x;
}
std::uint32_t bits(float x) {
    std::uint32_t b;
    std::memcpy(&b, &x, 4);
    return b;
}
double d(std::uint64_t b) {
    double x;
    std::memcpy(&x, &b, 8);
    return x;
}
std::uint64_t bits(double x) {
    std::uint64_t b;
    std::memcpy(&b, &x, 8);
    return b;
}
void lanes(const V &v, std::uint32_t (&l)[4]) {
    l[0] = static_cast<std::uint32_t>(v.lo);
    l[1] = static_cast<std::uint32_t>(v.lo >> 32);
    l[2] = static_cast<std::uint32_t>(v.hi);
    l[3] = static_cast<std::uint32_t>(v.hi >> 32);
}
V pack(const std::uint32_t (&l)[4]) { return V{l[0] | static_cast<std::uint64_t>(l[1]) << 32, l[2] | static_cast<std::uint64_t>(l[3]) << 32}; }

// Runs the module's function with (r, mem) as offsets from base. Returns false on an unknown op.
bool run_wasm(const Module &m, std::uintptr_t base, std::uint32_t r_off, std::uint32_t mem_off, std::string &why) {
    std::vector<V> locals(m.local_kind.size());
    locals[0].lo = r_off;
    locals[1].lo = mem_off;
    std::vector<V> st;
    struct C {
        std::size_t start, end;
        bool loop;
        std::size_t height;
    };
    std::vector<C> ctrl;
    ctrl.push_back({0, m.code.size(), false, 0});
    auto pop = [&] {
        V v = st.back();
        st.pop_back();
        return v;
    };
    auto u32 = [](const V &v) { return static_cast<std::uint32_t>(v.lo); };
    auto mem = [&](std::uint32_t addr, std::uint32_t off) { return reinterpret_cast<std::uint8_t *>(base + addr + off); };
    std::size_t pc = 0;
    auto branch = [&](std::uint32_t depth) {
        C target = ctrl[ctrl.size() - 1 - depth];
        st.resize(target.height);
        if (target.loop) {
            ctrl.resize(ctrl.size() - depth);
            pc = target.start + 1;
        } else {
            ctrl.resize(ctrl.size() - 1 - depth);
            pc = target.end + 1;
        }
    };
    for (std::uint64_t steps = 0; pc < m.code.size(); ++steps) {
        if (steps > 50'000'000) {
            why = "step limit";
            return false;
        }
        const Ins &in = m.code[pc];
        ++pc;
        switch (in.op) {
        case 0x01: break;
        case 0x02: case 0x03: ctrl.push_back({pc - 1, in.match, in.op == 0x03, st.size()}); break;
        case 0x04: {
            const std::uint32_t c = u32(pop());
            if (c) ctrl.push_back({pc - 1, in.match, false, st.size()});
            else pc = in.match + 1;
            break;
        }
        case 0x0B:
            if (ctrl.size() == 1) return true;
            st.resize(ctrl.back().height);
            ctrl.pop_back();
            break;
        case 0x0C: if (in.a + 1 >= ctrl.size()) return true; branch(in.a); break;
        case 0x0D: if (u32(pop())) { if (in.a + 1 >= ctrl.size()) return true; branch(in.a); } break;
        case 0x0E: {
            const std::uint32_t i = u32(pop());
            const std::uint32_t t = i < in.targets.size() - 1 ? in.targets[i] : in.targets.back();
            if (t + 1 >= ctrl.size()) return true;
            branch(t);
            break;
        }
        case 0x0F: return true;
        case 0x10: case 0x12: {
            const std::uint32_t fn = in.a;
            std::uint32_t a[4] = {};
            for (int k = kParams[fn] - 1; k >= 0; --k) a[k] = u32(pop());
            void *p = const_cast<void *>(vu::jit::a64_import(fn));
            std::uint32_t res = 0;
            switch (kParams[fn]) {
            case 0: res = reinterpret_cast<F0>(p)(); break;
            case 1: res = reinterpret_cast<F1>(p)(a[0]); break;
            case 2: res = reinterpret_cast<F2>(p)(a[0], a[1]); break;
            default: res = reinterpret_cast<F4>(p)(a[0], a[1], a[2], a[3]); break;
            }
            if (kResult[fn]) st.push_back(V{res, 0});
            if (in.op == 0x12) return true;
            break;
        }
        case 0x1A: st.pop_back(); break;
        case 0x1B: {
            const std::uint32_t c = u32(pop());
            const V b = pop(), a = pop();
            st.push_back(c ? a : b);
            break;
        }
        case 0x20: st.push_back(locals[in.a]); break;
        case 0x21: locals[in.a] = pop(); break;
        case 0x22: locals[in.a] = st.back(); break;
        case 0x28: { std::uint32_t v; std::memcpy(&v, mem(u32(pop()), in.b), 4); st.push_back(V{v, 0}); break; }
        case 0x29: { std::uint64_t v; std::memcpy(&v, mem(u32(pop()), in.b), 8); st.push_back(V{v, 0}); break; }
        case 0x2C: { std::int8_t v; std::memcpy(&v, mem(u32(pop()), in.b), 1); st.push_back(V{static_cast<std::uint32_t>(static_cast<std::int32_t>(v)), 0}); break; }
        case 0x2D: { std::uint8_t v; std::memcpy(&v, mem(u32(pop()), in.b), 1); st.push_back(V{v, 0}); break; }
        case 0x2E: { std::int16_t v; std::memcpy(&v, mem(u32(pop()), in.b), 2); st.push_back(V{static_cast<std::uint32_t>(static_cast<std::int32_t>(v)), 0}); break; }
        case 0x2F: { std::uint16_t v; std::memcpy(&v, mem(u32(pop()), in.b), 2); st.push_back(V{v, 0}); break; }
        case 0x36: case 0x37: case 0x3A: case 0x3B: {
            const V v = pop();
            const std::uint32_t a = u32(pop());
            const std::size_t n = in.op == 0x36 ? 4 : in.op == 0x37 ? 8 : in.op == 0x3A ? 1 : 2;
            std::memcpy(mem(a, in.b), &v.lo, n);
            break;
        }
        case 0x41: st.push_back(V{static_cast<std::uint32_t>(in.imm), 0}); break;
        case 0x42: st.push_back(V{static_cast<std::uint64_t>(in.imm), 0}); break;
        case 0x45: { const std::uint32_t a = u32(pop()); st.push_back(V{a == 0, 0}); break; }
        case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
            const std::uint32_t b = u32(pop()), a = u32(pop());
            const auto sa = static_cast<std::int32_t>(a), sb = static_cast<std::int32_t>(b);
            bool r = false;
            switch (in.op) {
            case 0x46: r = a == b; break;
            case 0x47: r = a != b; break;
            case 0x48: r = sa < sb; break;
            case 0x49: r = a < b; break;
            case 0x4A: r = sa > sb; break;
            case 0x4B: r = a > b; break;
            case 0x4C: r = sa <= sb; break;
            case 0x4D: r = a <= b; break;
            case 0x4E: r = sa >= sb; break;
            default: r = a >= b; break;
            }
            st.push_back(V{r, 0});
            break;
        }
        case 0x6A: case 0x6B: case 0x6C: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: {
            const std::uint32_t b = u32(pop()), a = u32(pop());
            std::uint32_t r = 0;
            switch (in.op) {
            case 0x6A: r = a + b; break;
            case 0x6B: r = a - b; break;
            case 0x6C: r = a * b; break;
            case 0x71: r = a & b; break;
            case 0x72: r = a | b; break;
            case 0x73: r = a ^ b; break;
            case 0x74: r = a << (b & 31); break;
            case 0x75: r = static_cast<std::uint32_t>(static_cast<std::int32_t>(a) >> (b & 31)); break;
            default: r = a >> (b & 31); break;
            }
            st.push_back(V{r, 0});
            break;
        }
        case 0x7C: case 0x7D: case 0x83: case 0x84: case 0x85: case 0x86: case 0x88: {
            const std::uint64_t b = pop().lo, a = pop().lo;
            std::uint64_t r = 0;
            switch (in.op) {
            case 0x7C: r = a + b; break;
            case 0x7D: r = a - b; break;
            case 0x83: r = a & b; break;
            case 0x84: r = a | b; break;
            case 0x85: r = a ^ b; break;
            case 0x86: r = a << (b & 63); break;
            default: r = a >> (b & 63); break;
            }
            st.push_back(V{r, 0});
            break;
        }
        case 0x9F: { const double a = d(pop().lo); st.push_back(V{bits(std::sqrt(a)), 0}); break; }
        case 0xA3: { const double b = d(pop().lo), a = d(pop().lo); st.push_back(V{bits(a / b), 0}); break; }
        case 0xA7: { const std::uint64_t a = pop().lo; st.push_back(V{static_cast<std::uint32_t>(a), 0}); break; }
        case 0xAC: { const std::uint32_t a = u32(pop()); st.push_back(V{static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(a))), 0}); break; }
        case 0xAD: { const std::uint32_t a = u32(pop()); st.push_back(V{a, 0}); break; }
        case 0xBD: case 0xBF: break;  // reinterpret: same bits
        case 0xC0: { const std::uint32_t a = u32(pop()); st.push_back(V{static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(a))), 0}); break; }
        case 0xC1: { const std::uint32_t a = u32(pop()); st.push_back(V{static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(a))), 0}); break; }
        case 0xFD: {
            std::uint32_t x[4], y[4], z[4];
            switch (in.sub) {
            case 0x00: { V v; std::memcpy(&v, mem(u32(pop()), in.b), 16); st.push_back(v); break; }
            case 0x0B: { const V v = pop(); std::memcpy(mem(u32(pop()), in.b), &v, 16); break; }
            case 0x0C: { V v; std::memcpy(&v, in.bytes, 16); st.push_back(v); break; }
            case 0x0D: {
                const V b = pop(), a = pop();
                std::uint8_t src[32], out[16];
                std::memcpy(src, &a, 16);
                std::memcpy(src + 16, &b, 16);
                for (int k = 0; k < 16; ++k) out[k] = src[in.bytes[k] & 31];
                V r;
                std::memcpy(&r, out, 16);
                st.push_back(r);
                break;
            }
            case 0x11: { const std::uint32_t a = u32(pop()); const std::uint32_t l[4] = {a, a, a, a}; st.push_back(pack(l)); break; }
            case 0x1B: { lanes(pop(), x); st.push_back(V{x[in.a & 3], 0}); break; }
            case 0x1C: { const std::uint32_t a = u32(pop()); lanes(pop(), x); x[in.a & 3] = a; st.push_back(pack(x)); break; }
            case 0x4E: case 0x4F: case 0x50: case 0x51: {
                const V b = pop(), a = pop();
                V r;
                if (in.sub == 0x4E) r = V{a.lo & b.lo, a.hi & b.hi};
                else if (in.sub == 0x4F) r = V{a.lo & ~b.lo, a.hi & ~b.hi};
                else if (in.sub == 0x50) r = V{a.lo | b.lo, a.hi | b.hi};
                else r = V{a.lo ^ b.lo, a.hi ^ b.hi};
                st.push_back(r);
                break;
            }
            case 0x52: {
                const V c = pop(), b = pop(), a = pop();
                st.push_back(V{(a.lo & c.lo) | (b.lo & ~c.lo), (a.hi & c.hi) | (b.hi & ~c.hi)});
                break;
            }
            case 0xA1: case 0xF8: case 0xFA: {
                lanes(pop(), x);
                for (int k = 0; k < 4; ++k) {
                    if (in.sub == 0xA1) z[k] = 0u - x[k];
                    else if (in.sub == 0xFA) z[k] = bits(static_cast<float>(static_cast<std::int32_t>(x[k])));
                    else {
                        const float v = f(x[k]);
                        z[k] = std::isnan(v) ? 0u
                               : v >= 2147483648.0f ? 0x7FFFFFFFu
                               : v < -2147483648.0f ? 0x80000000u
                                                    : static_cast<std::uint32_t>(static_cast<std::int32_t>(v));
                    }
                }
                st.push_back(pack(z));
                break;
            }
            default: {
                lanes(pop(), y);
                lanes(pop(), x);
                for (int k = 0; k < 4; ++k) {
                    const auto sa = static_cast<std::int32_t>(x[k]), sb = static_cast<std::int32_t>(y[k]);
                    const float fa = f(x[k]), fb = f(y[k]);
                    switch (in.sub) {
                    case 0x37: z[k] = x[k] == y[k] ? ~0u : 0u; break;
                    case 0x39: z[k] = sa < sb ? ~0u : 0u; break;
                    case 0x3B: z[k] = sa > sb ? ~0u : 0u; break;
                    case 0x3D: z[k] = sa <= sb ? ~0u : 0u; break;
                    case 0x3F: z[k] = sa >= sb ? ~0u : 0u; break;
                    case 0x41: z[k] = fa == fb ? ~0u : 0u; break;
                    case 0x43: z[k] = fa < fb ? ~0u : 0u; break;
                    case 0x44: z[k] = fa > fb ? ~0u : 0u; break;
                    case 0x45: z[k] = fa <= fb ? ~0u : 0u; break;
                    case 0x46: z[k] = fa >= fb ? ~0u : 0u; break;
                    case 0xAE: z[k] = x[k] + y[k]; break;
                    case 0xB1: z[k] = x[k] - y[k]; break;
                    case 0xB6: z[k] = static_cast<std::uint32_t>(std::min(sa, sb)); break;
                    case 0xB8: z[k] = static_cast<std::uint32_t>(std::max(sa, sb)); break;
                    case 0xE4: z[k] = bits(fa + fb); break;
                    case 0xE5: z[k] = bits(fa - fb); break;
                    case 0xE6: z[k] = bits(fa * fb); break;
                    default: {
                        char b[32];
                        std::snprintf(b, sizeof b, "simd 0x%X", in.sub);
                        why = b;
                        return false;
                    }
                    }
                }
                st.push_back(pack(z));
            }
            }
            break;
        }
        default: {
            char b[32];
            std::snprintf(b, sizeof b, "op 0x%02X", in.op);
            why = b;
            return false;
        }
        }
    }
    return true;
}

struct Snap {
    std::uint32_t entry, top, itop, regs_size;
    std::vector<std::uint8_t> code, mem;
    Vu0Regs regs;
};

struct Result {
    Vu0Regs regs;
    std::vector<std::uint8_t> mem;
    std::vector<std::uint8_t> flags;
    std::int64_t escape;
    std::vector<std::uint32_t> kicks;
};

constexpr std::size_t kFlagsSize = sizeof(vuops::FlagPipe);

void load_state(const Snap &s, const std::vector<std::uint8_t> &flags) {
    std::memcpy(g_vu1_code, s.code.data(), sizeof g_vu1_code);
    std::memcpy(g_vu1_mem, s.mem.data(), sizeof g_vu1_mem);
    vu::regs(1) = s.regs;
    vu::set_tops(1, s.top, s.itop);
    std::memcpy(vu::jit::a64_flags(), flags.data(), kFlagsSize);
}

Result capture(std::vector<std::uint32_t> &kicks) {
    Result r;
    r.regs = vu::regs(1);
    r.mem.assign(g_vu1_mem, g_vu1_mem + sizeof g_vu1_mem);
    r.flags.assign(static_cast<std::uint8_t *>(vu::jit::a64_flags()), static_cast<std::uint8_t *>(vu::jit::a64_flags()) + kFlagsSize);
    r.escape = vu::jit::g_escape;
    r.kicks = kicks;
    return r;
}

const char *field(std::size_t off) {
    if (off < offsetof(Vu0Regs, acc)) return "vf";
    if (off < offsetof(Vu0Regs, vi)) return "acc";
    if (off < offsetof(Vu0Regs, q)) return "vi";
    if (off < offsetof(Vu0Regs, p)) return "q";
    if (off < offsetof(Vu0Regs, i)) return "p";
    if (off < offsetof(Vu0Regs, r)) return "i";
    if (off < offsetof(Vu0Regs, mac)) return "r";
    if (off < offsetof(Vu0Regs, status)) return "mac";
    if (off < offsetof(Vu0Regs, clip)) return "status";
    if (off < offsetof(Vu0Regs, cmsar0)) return "clip";
    if (off < offsetof(Vu0Regs, tpc)) return "cmsar0";
    return "tpc";
}

int compare(const char *what, const Result &a, const Result &b) {
    int diffs = 0;
    const auto *ra = reinterpret_cast<const std::uint8_t *>(&a.regs), *rb = reinterpret_cast<const std::uint8_t *>(&b.regs);
    for (std::size_t k = 0; k < offsetof(Vu0Regs, tpc) + 4; k += 4)  // not the padding after tpc
        if (std::memcmp(ra + k, rb + k, 4) != 0 && diffs++ < 12) {
            std::uint32_t x, y;
            std::memcpy(&x, ra + k, 4);
            std::memcpy(&y, rb + k, 4);
            const std::size_t idx = k < offsetof(Vu0Regs, acc) ? k / 16 : 0;
            std::printf("    %s: %s%zu (+%zu) %08X vs %08X\n", what, field(k), idx, k % 16, x, y);
        }
    for (std::size_t k = 0; k < a.mem.size(); k += 4)
        if (std::memcmp(&a.mem[k], &b.mem[k], 4) != 0 && diffs++ < 24) {
            std::uint32_t x, y;
            std::memcpy(&x, &a.mem[k], 4);
            std::memcpy(&y, &b.mem[k], 4);
            std::printf("    %s: mem[%zu] (qw %zu.%zu) %08X vs %08X\n", what, k, k / 16, (k % 16) / 4, x, y);
        }
    for (std::size_t k = 0; k < a.flags.size(); ++k)
        if (a.flags[k] != b.flags[k]) {
            if (diffs++ < 30) std::printf("    %s: flag pipe byte %zu %02X vs %02X\n", what, k, a.flags[k], b.flags[k]);
        }
    if (a.escape != b.escape) {
        ++diffs;
        std::printf("    %s: escape %lld vs %lld\n", what, static_cast<long long>(a.escape), static_cast<long long>(b.escape));
    }
    if (a.kicks != b.kicks) {
        ++diffs;
        std::printf("    %s: kicks %zu vs %zu\n", what, a.kicks.size(), b.kicks.size());
    }
    return diffs;
}

}

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: vu_jit_a64_test <snap dir> [interp]\n");
        return 2;
    }
    const bool with_interp = argc > 2 && std::strcmp(argv[2], "interp") == 0;
    setenv("PS2_VU_INTERPRET", "1", 1);  // vu::interpret = the real interpreter, not the JIT
    const std::uintptr_t base = vu::jit::a64_base();
    if (!base) return 1;
    std::vector<std::filesystem::path> files;
    for (const auto &e : std::filesystem::directory_iterator(argv[1]))
        if (e.path().extension() == ".bin") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    int bad = 0;
    for (const auto &path : files) {
        std::ifstream in(path, std::ios::binary);
        Snap s;
        std::uint32_t head[4];
        in.read(reinterpret_cast<char *>(head), sizeof head);
        s.entry = head[0];
        s.top = head[1];
        s.itop = head[2];
        s.regs_size = head[3];
        if (s.regs_size != sizeof(Vu0Regs)) {
            std::printf("%s: register block size %u, expected %zu\n", path.filename().c_str(), s.regs_size, sizeof(Vu0Regs));
            continue;
        }
        s.code.resize(16384);
        s.mem.resize(16384);
        in.read(reinterpret_cast<char *>(s.code.data()), 16384);
        in.read(reinterpret_cast<char *>(s.mem.data()), 16384);
        in.read(reinterpret_cast<char *>(&s.regs), sizeof s.regs);
        std::vector<std::uint8_t> flags0(kFlagsSize, 0);

        std::vector<std::uint8_t> module;
        const vu::jit::Layout layout{static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(vu::jit::a64_flags()) - base)};
        if (!vu::jit::translate(s.code.data(), s.entry, layout, module)) {
            std::printf("%s: not translated\n", path.filename().c_str());
            continue;
        }
        const Module m = parse(module);
        const ps2::vu::ProgramFn fn = vu::jit::compile_a64(s.code.data(), s.entry);
        if (!fn || !m.ok) {
            std::printf("%s: compile failed\n", path.filename().c_str());
            continue;
        }
        const std::uint32_t r_off = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&vu::regs(1)) - base);
        const std::uint32_t m_off = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(g_vu1_mem) - base);

        std::vector<std::uint32_t> kicks;
        vu::jit::g_kicks = &kicks;

        load_state(s, flags0);
        kicks.clear();
        vu::jit::g_dry = true;
        vu::jit::g_escape = -1;
        fn(vu::regs(1), g_vu1_mem);
        const Result A = capture(kicks);

        load_state(s, flags0);
        kicks.clear();
        vu::jit::g_escape = -1;
        std::string why;
        if (!run_wasm(m, base, r_off, m_off, why)) {
            std::printf("%s: reference interpreter stopped: %s\n", path.filename().c_str(), why.c_str());
            continue;
        }
        const Result W = capture(kicks);
        vu::jit::g_dry = false;

        const int d = compare("A/W", A, W);
        std::printf("%s entry %04X module %zu bytes: ARM64 vs wasm %s (%d)\n", path.filename().c_str(), s.entry, module.size(), d ? "DIFFER" : "same", d);
        if (d) ++bad;
        if (with_interp) {
            load_state(s, flags0);
            vu::interpret(1, s.entry);
            std::vector<std::uint32_t> none;
            vu::jit::g_escape = A.escape;
            const Result I = capture(none);
            Result W2 = W;
            W2.flags = I.flags;  // the interpreter keeps its own flag pipe
            W2.kicks = none;
            const int di = compare("W/I", W2, I);
            std::printf("    wasm vs VU interpreter: %s (%d)\n", di ? "DIFFER" : "same", di);
        }
    }
    std::printf("%d of %zu snapshots differ (ARM64 vs wasm)\n", bad, files.size());
    return bad ? 1 : 0;
}

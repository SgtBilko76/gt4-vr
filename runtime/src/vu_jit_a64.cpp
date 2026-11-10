// VU1 JIT for ARM64 (Quest / Pico): compiles the WebAssembly module vu_jit.cpp's Translator emits for
// a VU1 program into ARM64 machine code. The translation logic (flags pipeline, Q latency, branch
// delay, shadows) stays the one the web build uses and checks against the interpreter; this file is
// only a back end for the small wasm subset that Translator writes. Anything else fails the compile
// (the program then runs in the interpreter).
//
// Addresses: wasm uses 32-bit offsets into its memory. Here they are offsets from g_base (x19), set
// below the VU1 registers, VU1 data memory and the flag pipe (all in the runtime's .bss, a few MB
// apart), so `load off` = [x19 + uxtw(addr) + off].
//
// Code shape: operand stack in registers by depth (integers x9..x15,x2..x8; f64/v128 v16..v31),
// locals in the frame (zeroed at entry like wasm locals), calls spill the stack below their args.
#if defined(__aarch64__) && !defined(__EMSCRIPTEN__)

#include "vu_jit.hpp"
#include "vu_jit_web.hpp"
#include "hw.hpp"
#include "vu_micro.hpp"
#include "ps2/vu_ops.hpp"

#include <sys/mman.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace ps2::vu::jit {

namespace {

vuops::FlagPipe g_fl;  // the flag pipe the translated programs use (Layout::flags)
std::uintptr_t g_base = 0;
bool g_base_ok = false;

template <typename T>
T *at(std::uint32_t offset) { return reinterpret_cast<T *>(g_base + offset); }

// ---- imports (vu_jit.cpp kImportNames order), wasm i32 arguments
extern "C" {
std::uint32_t vuj_mac(std::uint32_t fl) { return at<vuops::FlagPipe>(fl)->mac(); }
std::uint32_t vuj_status(std::uint32_t fl) { return at<vuops::FlagPipe>(fl)->status(); }
std::uint32_t vuj_clip(std::uint32_t fl) { return at<vuops::FlagPipe>(fl)->clip(); }
void vuj_force_mac(std::uint32_t fl, std::uint32_t r) {
    Vu0Regs *regs = at<Vu0Regs>(r);
    at<vuops::FlagPipe>(fl)->force_mac(regs->mac, regs->status);
}
void vuj_efu(std::uint32_t r, std::uint32_t index, std::uint32_t fs, std::uint32_t fsf) { vuops::efu(*at<Vu0Regs>(r), index, fs, fsf); }
void vuj_kick(std::uint32_t address) {
    if (g_dry) {
        g_kicks->push_back(address);
        return;
    }
    vu::kick(1, address);
}
std::uint32_t vuj_top() { return vu::top(1); }
std::uint32_t vuj_itop() { return vu::itop(1); }
void vuj_interpret(std::uint32_t r, std::uint32_t pc) {
    g_fl.flush_q(*at<Vu0Regs>(r));
    if (g_dry) {
        g_escape = pc;
        return;
    }
    vu::interpret(1, pc);
}
}

struct Import {
    const void *fn;
    std::uint8_t params;
    bool result;
};
const Import kImportFns[kImports] = {
    {reinterpret_cast<const void *>(&vuj_mac), 1, true},     {reinterpret_cast<const void *>(&vuj_status), 1, true},
    {reinterpret_cast<const void *>(&vuj_clip), 1, true},    {reinterpret_cast<const void *>(&vuj_force_mac), 2, false},
    {reinterpret_cast<const void *>(&vuj_efu), 4, false},    {reinterpret_cast<const void *>(&vuj_kick), 1, false},
    {reinterpret_cast<const void *>(&vuj_top), 0, true},     {reinterpret_cast<const void *>(&vuj_itop), 0, true},
    {reinterpret_cast<const void *>(&vuj_interpret), 2, false},
};

// ---- executable memory (never freed: a program may be on the stack when another compiles)
std::uint8_t *g_chunk = nullptr;
std::size_t g_chunk_used = 0, g_chunk_size = 0;

std::uint32_t *place(const std::vector<std::uint32_t> &code) {
    const std::size_t bytes = (code.size() * 4u + 63u) & ~std::size_t{63};
    if (!g_chunk || g_chunk_used + bytes > g_chunk_size) {
        const std::size_t size = std::max<std::size_t>(16u << 20, bytes);
        void *p = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) return nullptr;
        g_chunk = static_cast<std::uint8_t *>(p);
        g_chunk_size = size;
        std::fprintf(stderr, "[vu-jit] code chunk %p-%p\n", p, static_cast<void *>(g_chunk + size));
        g_chunk_used = 0;
    }
    auto *dst = reinterpret_cast<std::uint32_t *>(g_chunk + g_chunk_used);
    std::memcpy(dst, code.data(), code.size() * 4u);
    __builtin___clear_cache(reinterpret_cast<char *>(dst), reinterpret_cast<char *>(dst + code.size()));
    g_chunk_used += bytes;
    return dst;
}

enum Type : std::uint8_t { I32, I64, F64, V128 };
enum Cond : std::uint32_t { EQ = 0, NE = 1, HS = 2, LO = 3, HI = 8, LS = 9, GE = 10, LT = 11, GT = 12, LE = 13 };
constexpr unsigned SP = 31, X16 = 16, X17 = 17, BASE = 19;
constexpr unsigned kIntRegs[] = {9, 10, 11, 12, 13, 14, 15, 2, 3, 4, 5, 6, 7, 8};
constexpr unsigned kMaxDepth = 14;

struct Reader {
    const std::uint8_t *p, *end;
    bool ok = true;
    std::uint8_t byte() {
        if (p >= end) {
            ok = false;
            return 0;
        }
        return *p++;
    }
    std::uint32_t u32() {
        std::uint32_t v = 0;
        for (int s = 0; s < 35; s += 7) {
            const std::uint8_t c = byte();
            v |= static_cast<std::uint32_t>(c & 0x7Fu) << s;
            if (!(c & 0x80u)) return v;
        }
        ok = false;
        return v;
    }
    std::int64_t sleb(int bits) {
        std::int64_t v = 0;
        int s = 0;
        std::uint8_t c;
        do {
            c = byte();
            v |= static_cast<std::int64_t>(c & 0x7Fu) << s;
            s += 7;
        } while ((c & 0x80u) && s < 70 && ok);
        if (s < 64 && (c & 0x40u)) v |= -(static_cast<std::int64_t>(1) << s);
        if (bits == 32) v = static_cast<std::int32_t>(v);
        return v;
    }
};

enum Fix : std::uint8_t { kB26, kCond19, kTable };
struct Patch {
    std::size_t at;
    Fix kind;
    std::size_t table = 0;  // kTable: index of the table's first word
};

struct Ctrl {
    enum Kind { Block, Loop, If, Function } kind;
    std::size_t height;
    std::size_t loop_start = 0;
    std::vector<Patch> to_end;
    bool dead_at_start = false;
};

class Compiler {
public:
    bool compile(const std::vector<std::uint8_t> &module, std::vector<std::uint32_t> &out) {
        if (!locate_body(module)) return fail("module layout");
        prologue();
        ctrl_.push_back({Ctrl::Function, 0});
        Reader &r = rd_;
        while (!ctrl_.empty()) {
            if (!r.ok) return fail("truncated body");
            const std::uint8_t op = r.byte();
            if (!instruction(op)) return false;
        }
        // literal pool
        while (code_.size() % 4) code_.push_back(0xD4200000u);  // brk (never reached)
        for (const auto &[at, bytes] : pool_refs_) {
            const std::size_t word = code_.size();
            for (int k = 0; k < 4; ++k) code_.push_back(bytes[k]);
            const std::int64_t delta = static_cast<std::int64_t>(word) - static_cast<std::int64_t>(at);
            if (!fits(delta, 19)) range_error_ = true;
            code_[at] |= (static_cast<std::uint32_t>(delta) & 0x7FFFFu) << 5;
        }
        if (range_error_) return fail("program too large for 1 MB branches");
        out = std::move(code_);
        return true;
    }

    const char *error() const { return error_; }

private:
    // ---------- module
    bool locate_body(const std::vector<std::uint8_t> &m) {
        Reader r{m.data() + 8, m.data() + m.size()};
        if (m.size() < 8) return false;
        while (r.p < r.end) {
            const std::uint8_t id = r.byte();
            const std::uint32_t size = r.u32();
            if (!r.ok || r.p + size > r.end) return false;
            if (id != 10) {
                r.p += size;
                continue;
            }
            Reader s{r.p, r.p + size};
            if (s.u32() != 1) return false;
            const std::uint32_t body = s.u32();
            Reader b{s.p, s.p + body};
            // params: r, mem (i32)
            local_type_ = {I32, I32};
            const std::uint32_t groups = b.u32();
            for (std::uint32_t g = 0; g < groups; ++g) {
                const std::uint32_t n = b.u32();
                const std::uint8_t t = b.byte();
                const Type type = t == 0x7F ? I32 : t == 0x7E ? I64 : t == 0x7C ? F64 : t == 0x7B ? V128 : I32;
                if (t != 0x7F && t != 0x7E && t != 0x7C && t != 0x7B) return false;
                for (std::uint32_t k = 0; k < n; ++k) local_type_.push_back(type);
            }
            if (!b.ok) return false;
            std::uint32_t off = 0;
            for (Type t : local_type_) {
                if (t == V128) off = (off + 15u) & ~15u;
                local_off_.push_back(off);
                off += t == V128 ? 16u : 8u;
            }
            locals_size_ = (off + 15u) & ~15u;
            spill_off_ = locals_size_;
            args_off_ = spill_off_ + kMaxDepth * 16u;
            frame_ = (args_off_ + 4u * 8u + 15u) & ~15u;
            rd_ = b;
            return true;
        }
        return false;
    }

    bool fail(const char *why) {
        error_ = why;
        return false;
    }

    // ---------- emit
    void w(std::uint32_t i) { code_.push_back(i); }
    void movw(unsigned d, std::uint32_t v) {
        w(0x52800000u | (v & 0xFFFFu) << 5 | d);
        if (v >> 16) w(0x72A00000u | (v >> 16) << 5 | d);
    }
    void movx(unsigned d, std::uint64_t v) {
        w(0xD2800000u | (v & 0xFFFFu) << 5 | d);
        for (unsigned hw = 1; hw < 4; ++hw)
            if (const std::uint32_t part = (v >> (16 * hw)) & 0xFFFFu) w(0xF2800000u | hw << 21 | part << 5 | d);
    }
    void add_imm_x(unsigned d, unsigned n, std::uint32_t imm) {
        if (imm < 4096) {
            w(0x91000000u | imm << 10 | n << 5 | d);
            return;
        }
        movx(X17, imm);
        w(0x8B000000u | X17 << 16 | n << 5 | d);  // add xd, xn, x17 (n != sp here)
    }
    void sub_sp(std::uint32_t imm) {
        while (imm) {
            const std::uint32_t part = imm > 4095 ? 4080 : imm;
            w(0xD1000000u | part << 10 | SP << 5 | SP);
            imm -= part;
        }
    }
    void add_sp(std::uint32_t imm) {
        while (imm) {
            const std::uint32_t part = imm > 4095 ? 4080 : imm;
            w(0x91000000u | part << 10 | SP << 5 | SP);
            imm -= part;
        }
    }
    // frame slot access
    void frame_ld(Type t, unsigned reg, std::uint32_t off) {
        switch (t) {
        case I32: w(0xB9400000u | (off / 4) << 10 | SP << 5 | reg); break;
        case I64: w(0xF9400000u | (off / 8) << 10 | SP << 5 | reg); break;
        case F64: w(0xFD400000u | (off / 8) << 10 | SP << 5 | reg); break;
        case V128: w(0x3DC00000u | (off / 16) << 10 | SP << 5 | reg); break;
        }
    }
    void frame_st(Type t, unsigned reg, std::uint32_t off) {
        switch (t) {
        case I32: w(0xB9000000u | (off / 4) << 10 | SP << 5 | reg); break;
        case I64: w(0xF9000000u | (off / 8) << 10 | SP << 5 | reg); break;
        case F64: w(0xFD000000u | (off / 8) << 10 | SP << 5 | reg); break;
        case V128: w(0x3D800000u | (off / 16) << 10 | SP << 5 | reg); break;
        }
    }
    void vmov(unsigned d, unsigned n) {
        if (d != n) w(0x4EA01C00u | n << 16 | n << 5 | d);
    }

    void prologue() {
        w(0xA9BF7BFDu);  // stp x29, x30, [sp, #-16]!
        w(0x910003FDu);  // mov x29, sp
        w(0xA9BF53F3u);  // stp x19, x20, [sp, #-16]!
        sub_sp(frame_);
        movx(BASE, g_base);
        w(0x6F00E400u);  // movi v0.2d, #0
        for (std::uint32_t off = 0; off < locals_size_; off += 16) frame_st(V128, 0, off);
        // params as offsets from the base
        w(0xCB000000u | BASE << 16 | 0 << 5 | 0);  // sub x0, x0, x19
        w(0xCB000000u | BASE << 16 | 1 << 5 | 1);  // sub x1, x1, x19
        frame_st(I32, 0, local_off_[0]);
        frame_st(I32, 1, local_off_[1]);
    }
    void epilogue() {
        add_sp(frame_);
        w(0xA8C153F3u);  // ldp x19, x20, [sp], #16
        w(0xA8C17BFDu);  // ldp x29, x30, [sp], #16
        w(0xD65F03C0u);  // ret
    }

    // ---------- operand stack
    unsigned ireg(std::size_t d) const { return kIntRegs[d]; }
    unsigned vreg(std::size_t d) const { return 16u + static_cast<unsigned>(d); }
    unsigned reg_of(std::size_t d) const { return stack_[d] == I32 || stack_[d] == I64 ? ireg(d) : vreg(d); }
    bool push(Type t) {
        if (stack_.size() >= kMaxDepth) return fail("operand stack too deep");
        stack_.push_back(t);
        return true;
    }
    std::size_t top() const { return stack_.size() - 1; }
    bool need(std::size_t n) {
        if (stack_.size() < n + ctrl_.back().height) return fail("stack underflow");
        return true;
    }

    // x16 = base + uxtw(addr) (+ off when the access can't encode it)
    void address(unsigned areg) { w(0x8B204000u | areg << 16 | BASE << 5 | X16); }

    bool mem_int(bool store, unsigned size_log2, bool sign, std::uint32_t off) {
        // load: pop addr, push i32; store: pop value, pop addr
        if (store) {
            if (!need(2)) return false;
            const unsigned v = ireg(top()), a = ireg(top() - 1);
            address(a);
            const std::uint32_t scale = 1u << size_log2;
            if (off % scale || off / scale >= 4096) {
                add_imm_x(X16, X16, off);
                off = 0;
            }
            static const std::uint32_t kSt[4] = {0x39000000u, 0x79000000u, 0xB9000000u, 0xF9000000u};
            w(kSt[size_log2] | (off / scale) << 10 | X16 << 5 | v);
            stack_.pop_back();
            stack_.pop_back();
            return true;
        }
        if (!need(1)) return false;
        const unsigned a = ireg(top());
        address(a);
        const std::uint32_t scale = 1u << size_log2;
        if (off % scale || off / scale >= 4096) {
            add_imm_x(X16, X16, off);
            off = 0;
        }
        static const std::uint32_t kLd[4] = {0x39400000u, 0x79400000u, 0xB9400000u, 0xF9400000u};
        static const std::uint32_t kLdS[2] = {0x39C00000u, 0x79C00000u};  // ldrsb/ldrsh w
        const std::uint32_t enc = sign ? kLdS[size_log2] : kLd[size_log2];
        w(enc | (off / scale) << 10 | X16 << 5 | a);
        stack_.back() = size_log2 == 3 ? I64 : I32;
        return true;
    }

    bool vmem(bool store, std::uint32_t off) {
        if (store) {
            if (!need(2)) return false;
            const unsigned v = vreg(top()), a = ireg(top() - 1);
            address(a);
            if (off % 16 || off / 16 >= 4096) {
                add_imm_x(X16, X16, off);
                off = 0;
            }
            w(0x3D800000u | (off / 16) << 10 | X16 << 5 | v);
            stack_.pop_back();
            stack_.pop_back();
            return true;
        }
        if (!need(1)) return false;
        const unsigned a = ireg(top());
        address(a);
        if (off % 16 || off / 16 >= 4096) {
            add_imm_x(X16, X16, off);
            off = 0;
        }
        const std::size_t d = top();
        stack_.back() = V128;
        w(0x3DC00000u | (off / 16) << 10 | X16 << 5 | vreg(d));
        return true;
    }

    bool binary_int(std::uint32_t enc32, std::uint32_t enc64 = 0) {
        if (!need(2)) return false;
        const std::size_t b = top(), a = top() - 1;
        const bool wide = stack_[a] == I64;
        if (wide && !enc64) return fail("i64 op");
        w((wide ? enc64 : enc32) | ireg(b) << 16 | ireg(a) << 5 | ireg(a));
        stack_.pop_back();
        return true;
    }
    bool compare_int(Cond c, bool zero = false) {
        if (zero) {
            if (!need(1)) return false;
            const unsigned a = ireg(top());
            w((stack_.back() == I64 ? 0xF100001Fu : 0x7100001Fu) | a << 5);  // cmp a, #0
            w(0x1A9F07E0u | (c ^ 1u) << 12 | a);                            // cset
            stack_.back() = I32;
            return true;
        }
        if (!need(2)) return false;
        const std::size_t b = top(), a = top() - 1;
        w((stack_[a] == I64 ? 0xEB00001Fu : 0x6B00001Fu) | ireg(b) << 16 | ireg(a) << 5);
        w(0x1A9F07E0u | (c ^ 1u) << 12 | ireg(a));
        stack_.pop_back();
        stack_.back() = I32;
        return true;
    }
    bool binary_vec(std::uint32_t enc, bool swap = false) {
        if (!need(2)) return false;
        const std::size_t b = top(), a = top() - 1;
        const unsigned n = swap ? vreg(b) : vreg(a), m = swap ? vreg(a) : vreg(b);
        w(enc | m << 16 | n << 5 | vreg(a));
        stack_.pop_back();
        return true;
    }
    bool unary_vec(std::uint32_t enc) {
        if (!need(1)) return false;
        w(enc | vreg(top()) << 5 | vreg(top()));
        return true;
    }

    // ---------- control
    static bool fits(std::int64_t delta, int bits) { return delta >= -(std::int64_t{1} << (bits - 1)) && delta < (std::int64_t{1} << (bits - 1)); }
    void patch_here(const Patch &p) {
        const std::int64_t delta = static_cast<std::int64_t>(code_.size()) - static_cast<std::int64_t>(p.at);
        if (!fits(delta, p.kind == kB26 ? 26 : p.kind == kCond19 ? 19 : 30)) range_error_ = true;
        switch (p.kind) {
        case kB26: code_[p.at] |= static_cast<std::uint32_t>(delta) & 0x3FFFFFFu; break;
        case kCond19: code_[p.at] |= (static_cast<std::uint32_t>(delta) & 0x7FFFFu) << 5; break;
        case kTable:
            code_[p.at] = static_cast<std::uint32_t>((static_cast<std::int64_t>(code_.size()) - static_cast<std::int64_t>(p.table)) * 4);
            break;
        }
    }
    // A branch to label `depth`: backward to a loop start, else a patch at the block's end.
    void branch_to(std::uint32_t depth, std::uint32_t enc, Fix kind) {
        Ctrl &c = ctrl_[ctrl_.size() - 1 - depth];
        if (c.kind == Ctrl::Loop) {
            const std::int64_t delta = static_cast<std::int64_t>(c.loop_start) - static_cast<std::int64_t>(code_.size());
            if (!fits(delta, kind == kB26 ? 26 : 19)) range_error_ = true;
            if (kind == kB26) w(enc | (static_cast<std::uint32_t>(delta) & 0x3FFFFFFu));
            else w(enc | (static_cast<std::uint32_t>(delta) & 0x7FFFFu) << 5);
            return;
        }
        if (c.kind == Ctrl::Function) {  // br to the function: return
            if (kind == kB26) {
                epilogue();
                return;
            }
        }
        c.to_end.push_back({code_.size(), kind});
        w(enc);
    }
    void table_entry(std::uint32_t depth, std::size_t table) {
        Ctrl &c = ctrl_[ctrl_.size() - 1 - depth];
        if (c.kind == Ctrl::Loop) {
            w(static_cast<std::uint32_t>((static_cast<std::int64_t>(c.loop_start) - static_cast<std::int64_t>(table)) * 4));
            return;
        }
        c.to_end.push_back({code_.size(), kTable, table});
        w(0);
    }

    // call import f with its arguments on the stack
    bool call(std::uint32_t f, bool tail) {
        if (f >= kImports) return fail("call to a non-import");
        const Import &imp = kImportFns[f];
        if (!need(imp.params)) return false;
        const std::size_t base = stack_.size() - imp.params;
        for (std::size_t d = 0; d < base; ++d) frame_st(stack_[d], reg_of(d), spill_off_ + 16u * static_cast<std::uint32_t>(d));
        for (std::size_t k = 0; k < imp.params; ++k) frame_st(I64, ireg(base + k), args_off_ + 8u * static_cast<std::uint32_t>(k));
        for (std::size_t k = 0; k < imp.params; ++k) frame_ld(I32, static_cast<unsigned>(k), args_off_ + 8u * static_cast<std::uint32_t>(k));
        movx(X16, reinterpret_cast<std::uint64_t>(imp.fn));
        w(0xD63F0000u | X16 << 5);  // blr x16
        stack_.resize(base);
        if (tail) {
            epilogue();
            return true;
        }
        for (std::size_t d = 0; d < base; ++d) frame_ld(stack_[d], reg_of(d), spill_off_ + 16u * static_cast<std::uint32_t>(d));
        if (imp.result) {
            if (!push(I32)) return false;
            w(0x2A0003E0u | 0 << 16 | ireg(top()));  // mov w, w0
        }
        return true;
    }

    // Skips one instruction's immediates (dead code after br / br_table / return).
    bool skip_immediates(std::uint8_t op) {
        Reader &r = rd_;
        switch (op) {
        case 0x02: case 0x03: case 0x04: r.byte(); return true;
        case 0x0C: case 0x0D: case 0x10: case 0x12: case 0x20: case 0x21: case 0x22: r.u32(); return true;
        case 0x0E: {
            const std::uint32_t n = r.u32();
            for (std::uint32_t k = 0; k <= n; ++k) r.u32();
            return true;
        }
        case 0x41: r.sleb(32); return true;
        case 0x42: r.sleb(64); return true;
        default:
            if (op >= 0x28 && op <= 0x3E) {
                r.u32();
                r.u32();
            } else if (op == 0xFD) {
                const std::uint32_t sub = r.u32();
                if (sub == 0x00 || sub == 0x0B) {
                    r.u32();
                    r.u32();
                } else if (sub == 0x0C || sub == 0x0D) {
                    for (int k = 0; k < 16; ++k) r.byte();
                } else if (sub >= 0x15 && sub <= 0x22) {
                    r.byte();
                }
            }
            return true;
        }
    }

    bool instruction(std::uint8_t op) {
        Reader &r = rd_;
        if (dead_) {
            switch (op) {
            case 0x02: case 0x03: case 0x04:
                r.byte();
                ++dead_depth_;
                return true;
            case 0x0B:
                if (dead_depth_) {
                    --dead_depth_;
                    return true;
                }
                dead_ = false;
                return end_block();
            default: return skip_immediates(op);
            }
        }
        switch (op) {
        case 0x00: w(0xD4200000u); dead_ = true; return true;  // unreachable: brk
        case 0x01: return true;                                   // nop
        case 0x02: case 0x03: {
            if (r.byte() != 0x40) return fail("block with a result");
            Ctrl c{op == 0x02 ? Ctrl::Block : Ctrl::Loop, stack_.size()};
            c.loop_start = code_.size();
            ctrl_.push_back(std::move(c));
            return true;
        }
        case 0x04: {
            if (r.byte() != 0x40) return fail("if with a result");
            if (!need(1)) return false;
            const unsigned cond = ireg(top());
            stack_.pop_back();
            Ctrl c{Ctrl::If, stack_.size()};
            c.to_end.push_back({code_.size(), kCond19});
            w(0x34000000u | cond);  // cbz
            ctrl_.push_back(std::move(c));
            return true;
        }
        case 0x0B: return end_block();
        case 0x0C: branch_to(r.u32(), 0x14000000u, kB26); dead_ = true; return true;
        case 0x0D: {
            if (!need(1)) return false;
            const unsigned cond = ireg(top());
            stack_.pop_back();
            const std::uint32_t depth = r.u32();
            if (ctrl_[ctrl_.size() - 1 - depth].kind == Ctrl::Function) return fail("br_if to the function");
            branch_to(depth, 0x35000000u | cond, kCond19);  // cbnz
            return true;
        }
        case 0x0E: {
            const std::uint32_t n = r.u32();
            std::vector<std::uint32_t> targets(n + 1);
            for (auto &t : targets) t = r.u32();
            if (!need(1)) return false;
            const unsigned idx = ireg(top());
            stack_.pop_back();
            for (std::uint32_t t : targets)
                if (ctrl_[ctrl_.size() - 1 - t].kind == Ctrl::Function) return fail("br_table to the function");
            if (n < 4096) w(0x7100001Fu | n << 10 | idx << 5);  // cmp w, #n
            else {
                movw(X17, n);
                w(0x6B00001Fu | X17 << 16 | idx << 5);
            }
            branch_to(targets[n], 0x54000000u | HS, kCond19);  // b.hs default
            w(0x10000000u | (4u << 5) | X16);                  // adr x16, table (4 words ahead)
            w(0xB8A05800u | idx << 16 | X16 << 5 | X17);       // ldrsw x17, [x16, w, uxtw #2]
            w(0x8B110210u);                                     // add x16, x16, x17
            w(0xD61F0200u);                                     // br x16
            const std::size_t table = code_.size();
            for (std::uint32_t k = 0; k < n; ++k) table_entry(targets[k], table);
            dead_ = true;
            return true;
        }
        case 0x0F: epilogue(); dead_ = true; return true;
        case 0x10: return call(r.u32(), false);
        case 0x12: {
            const bool ok = call(r.u32(), true);
            dead_ = true;
            return ok;
        }
        case 0x1A:
            if (!need(1)) return false;
            stack_.pop_back();
            return true;
        case 0x1B: {
            if (!need(3)) return false;
            const std::size_t c = top(), b = top() - 1, a = top() - 2;
            const unsigned cond = ireg(c);
            w(0x7100001Fu | cond << 5);  // cmp cond, #0
            if (stack_[a] == V128 || stack_[a] == F64) {
                w(0x5A9F03E0u | (NE ^ 1u) << 12 | 0);  // csetm w0, ne
                w(0x4E040C00u | 0 << 5 | 0);           // dup v0.4s, w0
                w(0x6E601C00u | vreg(b) << 16 | vreg(a) << 5 | 0);  // bsl v0, va, vb
                vmov(vreg(a), 0);
            } else {
                w((stack_[a] == I64 ? 0x9A800000u : 0x1A800000u) | ireg(b) << 16 | NE << 12 | ireg(a) << 5 | ireg(a));
            }
            stack_.pop_back();
            stack_.pop_back();
            return true;
        }
        case 0x20: case 0x21: case 0x22: {
            const std::uint32_t l = r.u32();
            if (l >= local_type_.size()) return fail("local index");
            const Type t = local_type_[l];
            if (op == 0x20) {
                if (!push(t)) return false;
                frame_ld(t, reg_of(top()), local_off_[l]);
                return true;
            }
            if (!need(1)) return false;
            frame_st(t, reg_of(top()), local_off_[l]);
            if (op == 0x21) stack_.pop_back();
            return true;
        }
        case 0x28: case 0x29: case 0x2C: case 0x2D: case 0x2E: case 0x2F: {
            r.u32();
            const std::uint32_t off = r.u32();
            switch (op) {
            case 0x28: return mem_int(false, 2, false, off);
            case 0x29: return mem_int(false, 3, false, off);
            case 0x2C: return mem_int(false, 0, true, off);
            case 0x2D: return mem_int(false, 0, false, off);
            case 0x2E: return mem_int(false, 1, true, off);
            default: return mem_int(false, 1, false, off);
            }
        }
        case 0x36: case 0x37: case 0x3A: case 0x3B: {
            r.u32();
            const std::uint32_t off = r.u32();
            return mem_int(true, op == 0x36 ? 2 : op == 0x37 ? 3 : op == 0x3A ? 0 : 1, false, off);
        }
        case 0x41: {
            const auto v = static_cast<std::uint32_t>(r.sleb(32));
            if (!push(I32)) return false;
            movw(ireg(top()), v);
            return true;
        }
        case 0x42: {
            const auto v = static_cast<std::uint64_t>(r.sleb(64));
            if (!push(I64)) return false;
            movx(ireg(top()), v);
            return true;
        }
        case 0x45: return compare_int(EQ, true);
        case 0x46: return compare_int(EQ);
        case 0x47: return compare_int(NE);
        case 0x48: return compare_int(LT);
        case 0x49: return compare_int(LO);
        case 0x4A: return compare_int(GT);
        case 0x4B: return compare_int(HI);
        case 0x4C: return compare_int(LE);
        case 0x4D: return compare_int(LS);
        case 0x4E: return compare_int(GE);
        case 0x4F: return compare_int(HS);
        case 0x6A: return binary_int(0x0B000000u);
        case 0x6B: return binary_int(0x4B000000u);
        case 0x6C: return binary_int(0x1B007C00u);
        case 0x71: return binary_int(0x0A000000u);
        case 0x72: return binary_int(0x2A000000u);
        case 0x73: return binary_int(0x4A000000u);
        case 0x74: return binary_int(0x1AC02000u);
        case 0x75: return binary_int(0x1AC02800u);
        case 0x76: return binary_int(0x1AC02400u);
        case 0x7C: return binary_int(0, 0x8B000000u);
        case 0x7D: return binary_int(0, 0xCB000000u);
        case 0x83: return binary_int(0, 0x8A000000u);
        case 0x84: return binary_int(0, 0xAA000000u);
        case 0x85: return binary_int(0, 0xCA000000u);
        case 0x86: return binary_int(0, 0x9AC02000u);
        case 0x88: return binary_int(0, 0x9AC02400u);
        case 0x9F:  // f64.sqrt
            if (!need(1)) return false;
            w(0x1E61C000u | vreg(top()) << 5 | vreg(top()));
            return true;
        case 0xA3: {  // f64.div
            if (!need(2)) return false;
            const std::size_t b = top(), a = top() - 1;
            w(0x1E601800u | vreg(b) << 16 | vreg(a) << 5 | vreg(a));
            stack_.pop_back();
            return true;
        }
        case 0xA7: case 0xAD:  // i32.wrap_i64, i64.extend_i32_u: 32-bit move
            if (!need(1)) return false;
            w(0x2A0003E0u | ireg(top()) << 16 | ireg(top()));
            stack_.back() = op == 0xA7 ? I32 : I64;
            return true;
        case 0xAC:  // i64.extend_i32_s
            if (!need(1)) return false;
            w(0x93407C00u | ireg(top()) << 5 | ireg(top()));
            stack_.back() = I64;
            return true;
        case 0xBD:  // i64.reinterpret_f64
            if (!need(1)) return false;
            w(0x9E660000u | vreg(top()) << 5 | ireg(top()));
            stack_.back() = I64;
            return true;
        case 0xBF:  // f64.reinterpret_i64
            if (!need(1)) return false;
            w(0x9E670000u | ireg(top()) << 5 | vreg(top()));
            stack_.back() = F64;
            return true;
        case 0xC0: case 0xC1:  // i32.extend8_s / extend16_s
            if (!need(1)) return false;
            w((op == 0xC0 ? 0x13001C00u : 0x13003C00u) | ireg(top()) << 5 | ireg(top()));
            return true;
        case 0xFD: return simd(r.u32());
        default: {
            static char why[32];
            std::snprintf(why, sizeof why, "opcode 0x%02X", op);
            return fail(why);
        }
        }
    }

    bool end_block() {
        Ctrl c = std::move(ctrl_.back());
        ctrl_.pop_back();
        for (const Patch &p : c.to_end) patch_here(p);
        stack_.resize(c.height);
        if (c.kind == Ctrl::Function) epilogue();
        return true;
    }

    bool simd(std::uint32_t sub) {
        Reader &r = rd_;
        switch (sub) {
        case 0x00: { r.u32(); return vmem(false, r.u32()); }
        case 0x0B: { r.u32(); return vmem(true, r.u32()); }
        case 0x0C: {
            std::array<std::uint32_t, 4> v{};
            for (int k = 0; k < 16; ++k) v[k / 4] |= static_cast<std::uint32_t>(r.byte()) << (8 * (k % 4));
            if (!push(V128)) return false;
            pool_refs_.push_back({code_.size(), v});
            w(0x9C000000u | vreg(top()));  // ldr q, literal (patched)
            return true;
        }
        case 0x0D: {
            std::array<std::uint32_t, 4> lanes{};
            for (int k = 0; k < 16; ++k) lanes[k / 4] |= static_cast<std::uint32_t>(r.byte()) << (8 * (k % 4));
            if (!need(2)) return false;
            const std::size_t b = top(), a = top() - 1;
            vmov(0, vreg(a));
            vmov(1, vreg(b));
            pool_refs_.push_back({code_.size(), lanes});
            w(0x9C000000u | 2);                                  // ldr q2, indices
            w(0x4E002000u | 2 << 16 | 0 << 5 | vreg(a));         // tbl va.16b, {v0, v1}, v2
            stack_.pop_back();
            return true;
        }
        case 0x11:  // i32x4.splat
            if (!need(1)) return false;
            w(0x4E040C00u | ireg(top()) << 5 | vreg(top()));
            stack_.back() = V128;
            return true;
        case 0x1B: {  // i32x4.extract_lane
            const std::uint32_t lane = r.byte();
            if (!need(1) || lane > 3) return fail("extract lane");
            w(0x0E003C00u | ((lane << 3) | 4u) << 16 | vreg(top()) << 5 | ireg(top()));
            stack_.back() = I32;
            return true;
        }
        case 0x1C: {  // i32x4.replace_lane
            const std::uint32_t lane = r.byte();
            if (!need(2) || lane > 3) return fail("replace lane");
            const std::size_t x = top(), v = top() - 1;
            w(0x4E001C00u | ((lane << 3) | 4u) << 16 | ireg(x) << 5 | vreg(v));
            stack_.pop_back();
            return true;
        }
        case 0x37: return binary_vec(0x6EA08C00u);               // i32x4.eq
        case 0x39: return binary_vec(0x4EA03400u, true);         // lt_s = cmgt(b, a)
        case 0x3B: return binary_vec(0x4EA03400u);               // gt_s
        case 0x3D: return binary_vec(0x4EA03C00u, true);         // le_s = cmge(b, a)
        case 0x3F: return binary_vec(0x4EA03C00u);               // ge_s
        case 0x41: return binary_vec(0x4E20E400u);               // f32x4.eq
        case 0x43: return binary_vec(0x6EA0E400u, true);         // f32x4.lt = fcmgt(b, a)
        case 0x44: return binary_vec(0x6EA0E400u);               // f32x4.gt
        case 0x45: return binary_vec(0x6E20E400u, true);         // f32x4.le = fcmge(b, a)
        case 0x46: return binary_vec(0x6E20E400u);               // f32x4.ge
        case 0x4E: return binary_vec(0x4E201C00u);               // and
        case 0x4F: return binary_vec(0x4E601C00u);               // andnot = bic
        case 0x50: return binary_vec(0x4EA01C00u);               // or
        case 0x51: return binary_vec(0x6E201C00u);               // xor
        case 0x52: {                                              // bitselect(a, b, c) = (a & c) | (b & ~c)
            if (!need(3)) return false;
            const std::size_t c = top(), b = top() - 1, a = top() - 2;
            vmov(0, vreg(c));
            w(0x6E601C00u | vreg(b) << 16 | vreg(a) << 5 | 0);   // bsl v0, va, vb
            vmov(vreg(a), 0);
            stack_.pop_back();
            stack_.pop_back();
            return true;
        }
        case 0xA1: return unary_vec(0x6EA0B800u);                // i32x4.neg
        case 0xAE: return binary_vec(0x4EA08400u);               // i32x4.add
        case 0xB1: return binary_vec(0x6EA08400u);               // i32x4.sub
        case 0xB6: return binary_vec(0x4EA06C00u);               // i32x4.min_s
        case 0xB8: return binary_vec(0x4EA06400u);               // i32x4.max_s
        case 0xE4: return binary_vec(0x4E20D400u);               // f32x4.add
        case 0xE5: return binary_vec(0x4EA0D400u);               // f32x4.sub
        case 0xE6: return binary_vec(0x6E20DC00u);               // f32x4.mul
        case 0xF8: return unary_vec(0x4EA1B800u);                // i32x4.trunc_sat_f32x4_s
        case 0xFA: return unary_vec(0x4E21D800u);                // f32x4.convert_i32x4_s
        default: {
            static char why[32];
            std::snprintf(why, sizeof why, "simd 0x%X", sub);
            return fail(why);
        }
        }
    }

    Reader rd_{nullptr, nullptr};
    std::vector<Type> local_type_;
    std::vector<std::uint32_t> local_off_;
    std::uint32_t locals_size_ = 0, spill_off_ = 0, args_off_ = 0, frame_ = 0;
    std::vector<std::uint32_t> code_;
    std::vector<std::pair<std::size_t, std::array<std::uint32_t, 4>>> pool_refs_;
    std::vector<Type> stack_;
    std::vector<Ctrl> ctrl_;
    bool dead_ = false;
    bool range_error_ = false;
    unsigned dead_depth_ = 0;
    const char *error_ = "";
};

bool init_base() {
    static bool done = false;
    if (done) return g_base_ok;
    done = true;
    const auto lo = [](const void *p) { return reinterpret_cast<std::uintptr_t>(p); };
    const std::uintptr_t a = lo(&regs(1)), b = lo(g_vu1_mem), c = lo(&g_fl);
    const std::uintptr_t low = std::min({a, b, c}) & ~std::uintptr_t{0xFFF};
    const std::uintptr_t high = std::max({a + sizeof(Vu0Regs), b + sizeof g_vu1_mem, c + sizeof g_fl});
    g_base = low;
    g_base_ok = high - low < (std::uintptr_t{1} << 31);
    std::fprintf(stderr, "[vu-jit] ARM64 VU1 JIT %s (span %zu KB)\n", g_base_ok ? "on" : "off: data too far apart",
                 static_cast<std::size_t>((high - low) >> 10));
    return g_base_ok;
}

}

// For tests/vu_jit_a64_test.cpp: the data base, the flag pipe and the import functions.
std::uintptr_t a64_base() { return init_base() ? g_base : 0; }
void *a64_flags() { return &g_fl; }
const void *a64_import(std::uint32_t i) { return i < kImports ? kImportFns[i].fn : nullptr; }

ProgramFn compile_a64(const std::uint8_t *code, std::uint32_t entry) {
    if (!init_base()) return nullptr;
    std::vector<std::uint8_t> module;
    const Layout layout{static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&g_fl) - g_base)};
    if (!translate(code, entry, layout, module)) return nullptr;
    Compiler c;
    std::vector<std::uint32_t> words;
    if (!c.compile(module, words)) {
        static unsigned reported = 0;
        if (reported++ < 10) std::fprintf(stderr, "[vu-jit] program at 0x%04X not compiled: %s\n", entry, c.error());
        return nullptr;
    }
    std::uint32_t *fn = place(words);
    return reinterpret_cast<ProgramFn>(fn);
}

}

#endif

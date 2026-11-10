// EE JIT for ARM64 (Quest / Pico): translates a block of R5900 code (ee_core.hpp) into ARM64 machine
// code on first use. Common integer, load/store and branch instructions are emitted natively; every
// other instruction calls the interpreter's simple() for that one instruction, so behaviour stays
// identical to ee_interp.cpp (same block boundaries, same budget charges; PS2_EE_JIT=0 runs the
// interpreter for comparison).
//
// Register use in generated code: x19 = Cpu*, x20 = g_ram, w21 = branch condition / jump target kept
// across the delay slot, x0-x17 scratch. Guest registers live in the Cpu struct (no allocation yet).
// A block returns the packed Exit (ee_core.hpp) in x0 and leaves c.pc set.
#if defined(__aarch64__)

#include "ee_core.hpp"
#include "ps2/generated.hpp"
#include "ps2/memory.hpp"
#include "ps2/system.hpp"
#include "ps2/vu.hpp"

#include <sys/mman.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

namespace ps2::ee {

namespace {

using recomp::Flow;
using recomp::Insn;
using enum recomp::Op;

// ---- helpers called from generated code (C ABI, arguments in x0..x3)
extern "C" {
std::uint64_t ee_h_ld8(std::uint32_t a) { return ld8(a); }
std::uint64_t ee_h_ld16(std::uint32_t a) { return ld16(a); }
std::uint64_t ee_h_ld32(std::uint32_t a) { return ld32(a); }
std::uint64_t ee_h_ld64(std::uint32_t a) { return ld64(a); }
void ee_h_ld128(std::uint32_t a, Reg128 *out) { ld128(a, *out); }
void ee_h_st8(std::uint32_t a, std::uint64_t v) { st8(a, static_cast<std::uint8_t>(v)); }
void ee_h_st16(std::uint32_t a, std::uint64_t v) { st16(a, static_cast<std::uint16_t>(v)); }
void ee_h_st32(std::uint32_t a, std::uint64_t v) { st32(a, static_cast<std::uint32_t>(v)); }
void ee_h_st64(std::uint32_t a, std::uint64_t v) { st64(a, v); }
void ee_h_st128(std::uint32_t a, const Reg128 *v) { st128(a, *v); }
// PS2_EE_JIT_STATS=1: count the instructions that take the simple() path, by opcode (printed every
// 2^20 calls) - shows which ops are worth translating natively.
std::uint64_t g_simple_count[1024];
std::uint64_t g_simple_total = 0;
std::uint64_t ee_h_simple(Cpu *c, const Insn *i) {
    static const bool stats = std::getenv("PS2_EE_JIT_STATS") != nullptr;
    if (stats) {
        ++g_simple_count[static_cast<unsigned>(i->op) & 1023u];
        if ((++g_simple_total & ((1u << 20) - 1)) == 0) {
            std::fprintf(stderr, "[ee-jit] simple() calls %llu:", static_cast<unsigned long long>(g_simple_total));
            for (unsigned k = 0; k < 1024; ++k)
                if (g_simple_count[k] * 200 >= g_simple_total)
                    std::fprintf(stderr, " %s:%llu", recomp::name(static_cast<recomp::Op>(k)), static_cast<unsigned long long>(g_simple_count[k]));
            std::fprintf(stderr, "\n");
        }
    }
    return simple(*c, *i) ? 1u : 0u;
}
std::uint64_t ee_h_cond(Cpu *c, const Insn *i) { return condition(*c, *i) ? 1u : 0u; }
void ee_h_stop(Cpu *c, std::uint32_t pc, const char *what) { stop(*c, pc, what); }
void ee_h_cache() { code_may_change(); }
// A call from translated code, nested like generated code's. 1 = it returned to `ret`.
std::uint64_t ee_h_call(Cpu *c, std::uint32_t target, std::uint32_t ret) {
    const std::uint32_t d = g_depth;
    c->pc = target;
    call(*c, target, d + 1u);
    g_depth = d;
    return c->pc == ret ? 1u : 0u;
}
// Unaligned loads / stores (memory.hpp), DIV.S, and the frequent COP2 macro ops, called directly
// (the same code as ee_interp.cpp's cases, without simple()'s dispatch). None of them stops.
void ee_x_ldl(Reg128 *rt, std::uint32_t a) { ldl(*rt, a); }
void ee_x_ldr(Reg128 *rt, std::uint32_t a) { ldr(*rt, a); }
void ee_x_lwl(Reg128 *rt, std::uint32_t a) { lwl(*rt, a); }
void ee_x_lwr(Reg128 *rt, std::uint32_t a) { lwr(*rt, a); }
void ee_x_sdl(const Reg128 *rt, std::uint32_t a) { sdl(*rt, a); }
void ee_x_sdr(const Reg128 *rt, std::uint32_t a) { sdr(*rt, a); }
void ee_x_swl(const Reg128 *rt, std::uint32_t a) { swl(*rt, a); }
void ee_x_swr(const Reg128 *rt, std::uint32_t a) { swr(*rt, a); }
void ee_x_divs(Cpu *c, std::uint32_t fd, std::uint32_t a, std::uint32_t b) { fpu_div(*c, fd, a, b); }
#define BC v.vf[i->ft()].w[i->bc()]
void ee_v_maddabc(Vu0Regs *pv, const Insn *i) { Vu0Regs &v = *pv; vu::maddabc(v, i->dest(), i->fs(), BC); }
void ee_v_mulabc(Vu0Regs *pv, const Insn *i) { Vu0Regs &v = *pv; vu::mulabc(v, i->dest(), i->fs(), BC); }
void ee_v_maddbc(Vu0Regs *pv, const Insn *i) { Vu0Regs &v = *pv; vu::maddbc(v, i->dest(), i->fd(), i->fs(), BC); }
void ee_v_mulbc(Vu0Regs *pv, const Insn *i) { Vu0Regs &v = *pv; vu::mulbc(v, i->dest(), i->fd(), i->fs(), BC); }
void ee_v_addbc(Vu0Regs *pv, const Insn *i) { Vu0Regs &v = *pv; vu::addbc(v, i->dest(), i->fd(), i->fs(), BC); }
void ee_v_subbc(Vu0Regs *pv, const Insn *i) { Vu0Regs &v = *pv; vu::subbc(v, i->dest(), i->fd(), i->fs(), BC); }
void ee_v_mul(Vu0Regs *pv, const Insn *i) { Vu0Regs &v = *pv; vu::mul(v, i->dest(), i->fd(), i->fs(), i->ft()); }
void ee_v_add(Vu0Regs *pv, const Insn *i) { Vu0Regs &v = *pv; vu::add(v, i->dest(), i->fd(), i->fs(), i->ft()); }
void ee_v_sub(Vu0Regs *pv, const Insn *i) { Vu0Regs &v = *pv; vu::sub(v, i->dest(), i->fd(), i->fs(), i->ft()); }
void ee_v_madd(Vu0Regs *pv, const Insn *i) { Vu0Regs &v = *pv; vu::madd(v, i->dest(), i->fd(), i->fs(), i->ft()); }
void ee_v_mula(Vu0Regs *pv, const Insn *i) { Vu0Regs &v = *pv; vu::mula(v, i->dest(), i->fs(), i->ft()); }
void ee_v_madda(Vu0Regs *pv, const Insn *i) { Vu0Regs &v = *pv; vu::madda(v, i->dest(), i->fs(), i->ft()); }
void ee_v_clip(Vu0Regs *pv, const Insn *i) { vu::clip(*pv, i->fs(), i->ft()); }
#undef BC
std::uint32_t ee_fp_add(std::uint32_t a, std::uint32_t b) { return fp::add(a, b); }
std::uint32_t ee_fp_sub(std::uint32_t a, std::uint32_t b) { return fp::sub(a, b); }
std::uint32_t ee_fp_mul(std::uint32_t a, std::uint32_t b) { return fp::fpu_mul(a, b); }
std::uint32_t ee_fp_madd(std::uint32_t acc, std::uint32_t a, std::uint32_t b) { return fp::add(acc, fp::fpu_mul(a, b)); }
std::uint32_t ee_fp_msub(std::uint32_t acc, std::uint32_t a, std::uint32_t b) { return fp::sub(acc, fp::fpu_mul(a, b)); }
std::uint32_t ee_fp_max(std::uint32_t a, std::uint32_t b) { return fp::max(a, b); }
std::uint32_t ee_fp_min(std::uint32_t a, std::uint32_t b) { return fp::min(a, b); }
std::uint32_t ee_fp_cvtws(std::uint32_t a) { return fp::cvt_w_s(a); }
std::uint32_t ee_fp_cvtsw(std::uint32_t a) { return fp::cvt_s_w(a); }
std::uint32_t ee_fp_sqrt(std::uint32_t a) { return fp::sqrt(a); }
// c.cond.s: 0 eq, 1 lt, 2 le
void ee_fp_cmp(Cpu *c, std::uint32_t a, std::uint32_t b, std::uint32_t mode) {
    const std::int32_t x = fp::key(a), y = fp::key(b);
    fpu_compare(*c, mode == 0 ? x == y : mode == 1 ? x < y : x <= y);
}
}

constexpr std::uint32_t kOffR = offsetof(Cpu, r);
constexpr std::uint32_t kOffHi = offsetof(Cpu, hi);
constexpr std::uint32_t kOffLo = offsetof(Cpu, lo);
constexpr std::uint32_t kOffPc = offsetof(Cpu, pc);
constexpr std::uint32_t kOffBudget = offsetof(Cpu, budget);
constexpr std::uint32_t kOffF = offsetof(Cpu, f);
constexpr std::uint32_t kOffFcr31 = offsetof(Cpu, fcr31);
constexpr std::uint32_t kOffFacc = offsetof(Cpu, facc);
constexpr std::uint32_t fr(unsigned r) { return kOffF + r * 4u; }
constexpr std::uint32_t gr(unsigned r) { return kOffR + r * 16u; }
constexpr std::uint32_t kOffVu0 = offsetof(Cpu, vu0);
constexpr std::uint32_t vfr(unsigned r) { return kOffVu0 + static_cast<std::uint32_t>(offsetof(Vu0Regs, vf)) + r * 16u; }
constexpr std::uint32_t vir(unsigned r) { return kOffVu0 + static_cast<std::uint32_t>(offsetof(Vu0Regs, vi)) + r * 2u; }
static_assert(kOffVu0 < 4096 && vfr(31) + 16 < 4096 * 4 && vir(15) < 4096 * 2, "VU0 registers within the immediate offsets");

enum Cond : std::uint32_t { EQ = 0, NE = 1, HS = 2, LO = 3, GE = 10, LT = 11, GT = 12, LE = 13 };
constexpr unsigned ZR = 31, X16 = 16, C = 19, RAM = 20, T = 21;

// ---- executable memory: chunks are never freed while the game runs (a helper may re-enter the
// dispatcher and compile while an older block is still on the stack).
struct Chunk {
    std::uint8_t *base = nullptr;
    std::size_t size = 0, used = 0;
};
std::vector<Chunk> g_chunks;
constexpr std::size_t kChunk = 32u << 20;

std::uint32_t *reserve(std::size_t bytes) {
    if (g_chunks.empty() || g_chunks.back().used + bytes > g_chunks.back().size) {
        void *p = mmap(nullptr, kChunk, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) return nullptr;
        g_chunks.push_back({static_cast<std::uint8_t *>(p), kChunk, 0});
        std::fprintf(stderr, "[ee-jit] code chunk %p-%p\n", p, static_cast<void *>(static_cast<std::uint8_t *>(p) + kChunk));
    }
    Chunk &k = g_chunks.back();
    return reinterpret_cast<std::uint32_t *>(k.base + k.used);
}

class Asm {
public:
    explicit Asm(std::uint32_t *p) : start_(p), p_(p) {}
    std::uint32_t *start() const { return start_; }
    std::uint32_t *here() const { return p_; }
    std::size_t bytes() const { return static_cast<std::size_t>(p_ - start_) * 4u; }
    void w(std::uint32_t i) { *p_++ = i; }

    // moves and constants
    void mov_x(unsigned d, unsigned m) { w(0xAA0003E0u | m << 16 | d); }  // orr xd, xzr, xm
    void movz_x(unsigned d, std::uint32_t imm16, unsigned hw) { w(0xD2800000u | hw << 21 | imm16 << 5 | d); }
    void movk_x(unsigned d, std::uint32_t imm16, unsigned hw) { w(0xF2800000u | hw << 21 | imm16 << 5 | d); }
    void imm64(unsigned d, std::uint64_t v) {
        movz_x(d, v & 0xFFFFu, 0);
        for (unsigned hw = 1; hw < 4; ++hw)
            if (const std::uint32_t part = (v >> (16 * hw)) & 0xFFFFu) movk_x(d, part, hw);
    }
    void imm32(unsigned d, std::uint32_t v) {  // zero-extended into xd
        w(0x52800000u | (v & 0xFFFFu) << 5 | d);
        if (v >> 16) w(0x72A00000u | (v >> 16) << 5 | d);
    }
    // loads/stores relative to a base register (unsigned scaled offset)
    void ldr_x(unsigned t, unsigned n, std::uint32_t off) { w(0xF9400000u | (off / 8) << 10 | n << 5 | t); }
    void str_x(unsigned t, unsigned n, std::uint32_t off) { w(0xF9000000u | (off / 8) << 10 | n << 5 | t); }
    void ldr_w(unsigned t, unsigned n, std::uint32_t off) { w(0xB9400000u | (off / 4) << 10 | n << 5 | t); }
    void str_w(unsigned t, unsigned n, std::uint32_t off) { w(0xB9000000u | (off / 4) << 10 | n << 5 | t); }
    void ldp_x(unsigned t1, unsigned t2, unsigned n, std::int32_t off) { w(0xA9400000u | ((off / 8) & 0x7F) << 15 | t2 << 10 | n << 5 | t1); }
    void stp_x(unsigned t1, unsigned t2, unsigned n, std::int32_t off) { w(0xA9000000u | ((off / 8) & 0x7F) << 15 | t2 << 10 | n << 5 | t1); }
    // data processing (register); register 31 reads as zero
    void add_x(unsigned d, unsigned n, unsigned m) { w(0x8B000000u | m << 16 | n << 5 | d); }
    void sub_x(unsigned d, unsigned n, unsigned m) { w(0xCB000000u | m << 16 | n << 5 | d); }
    void add_w(unsigned d, unsigned n, unsigned m) { w(0x0B000000u | m << 16 | n << 5 | d); }
    void sub_w(unsigned d, unsigned n, unsigned m) { w(0x4B000000u | m << 16 | n << 5 | d); }
    void and_x(unsigned d, unsigned n, unsigned m) { w(0x8A000000u | m << 16 | n << 5 | d); }
    void orr_x(unsigned d, unsigned n, unsigned m) { w(0xAA000000u | m << 16 | n << 5 | d); }
    void eor_x(unsigned d, unsigned n, unsigned m) { w(0xCA000000u | m << 16 | n << 5 | d); }
    void orn_x(unsigned d, unsigned n, unsigned m) { w(0xAA200000u | m << 16 | n << 5 | d); }
    void lslv_w(unsigned d, unsigned n, unsigned m) { w(0x1AC02000u | m << 16 | n << 5 | d); }
    void lsrv_w(unsigned d, unsigned n, unsigned m) { w(0x1AC02400u | m << 16 | n << 5 | d); }
    void asrv_w(unsigned d, unsigned n, unsigned m) { w(0x1AC02800u | m << 16 | n << 5 | d); }
    void lslv_x(unsigned d, unsigned n, unsigned m) { w(0x9AC02000u | m << 16 | n << 5 | d); }
    void lsrv_x(unsigned d, unsigned n, unsigned m) { w(0x9AC02400u | m << 16 | n << 5 | d); }
    void asrv_x(unsigned d, unsigned n, unsigned m) { w(0x9AC02800u | m << 16 | n << 5 | d); }
    void add_wi(unsigned d, unsigned n, std::uint32_t imm12) { w(0x11000000u | imm12 << 10 | n << 5 | d); }
    void sub_wi(unsigned d, unsigned n, std::uint32_t imm12) { w(0x51000000u | imm12 << 10 | n << 5 | d); }
    void ubfm_w(unsigned d, unsigned n, unsigned immr, unsigned imms) { w(0x53000000u | immr << 16 | imms << 10 | n << 5 | d); }
    void sbfm_w(unsigned d, unsigned n, unsigned immr, unsigned imms) { w(0x13000000u | immr << 16 | imms << 10 | n << 5 | d); }
    void ubfm_x(unsigned d, unsigned n, unsigned immr, unsigned imms) { w(0xD3400000u | immr << 16 | imms << 10 | n << 5 | d); }
    void sbfm_x(unsigned d, unsigned n, unsigned immr, unsigned imms) { w(0x93400000u | immr << 16 | imms << 10 | n << 5 | d); }
    void sxtw(unsigned d, unsigned n) { sbfm_x(d, n, 0, 31); }
    void sxth(unsigned d, unsigned n) { sbfm_x(d, n, 0, 15); }
    void sxtb(unsigned d, unsigned n) { sbfm_x(d, n, 0, 7); }
    void and_w_not15(unsigned d, unsigned n) { w(0x121C6C00u | n << 5 | d); }  // and wd, wn, #0xFFFFFFF0 (28 ones, ror 28)
    void eor_w1(unsigned d, unsigned n) { w(0x52000000u | n << 5 | d); }        // eor wd, wn, #1
    void cmp_x(unsigned n, unsigned m) { w(0xEB00001Fu | m << 16 | n << 5); }
    void cmp_w(unsigned n, unsigned m) { w(0x6B00001Fu | m << 16 | n << 5); }
    std::uint32_t *bcond(Cond c) { w(0x54000000u | c); return p_ - 1; }
    void br(unsigned n) { w(0xD61F0000u | n << 5); }
    void smull(unsigned d, unsigned n, unsigned m) { w(0x9B207C00u | m << 16 | n << 5 | d); }
    void umull(unsigned d, unsigned n, unsigned m) { w(0x9BA07C00u | m << 16 | n << 5 | d); }
    void and_w_abs(unsigned d, unsigned n) { w(0x12007800u | n << 5 | d); }   // and wd, wn, #0x7FFFFFFF
    void eor_w_sign(unsigned d, unsigned n) { w(0x52010000u | n << 5 | d); }  // eor wd, wn, #0x80000000
    void cset_w(unsigned d, Cond c) { w(0x1A9F07E0u | (c ^ 1u) << 12 | d); }
    void csel_x(unsigned d, unsigned n, unsigned m, Cond c) { w(0x9A800000u | m << 16 | c << 12 | n << 5 | d); }
    // memory with a 32-bit guest address zero-extended: [x20, wm, uxtw]
    void ram(std::uint32_t op, unsigned t, unsigned m) { w(op | m << 16 | RAM << 5 | t); }
    void ldrh_w(unsigned t, unsigned n, std::uint32_t off) { w(0x79400000u | (off / 2) << 10 | n << 5 | t); }
    void strh_w(unsigned t, unsigned n, std::uint32_t off) { w(0x79000000u | (off / 2) << 10 | n << 5 | t); }
    void ldr_q(unsigned t, unsigned n, std::uint32_t off) { w(0x3DC00000u | (off / 16) << 10 | n << 5 | t); }
    void str_q(unsigned t, unsigned n, std::uint32_t off) { w(0x3D800000u | (off / 16) << 10 | n << 5 | t); }
    void movi_zero_q(unsigned d) { w(0x6F00E400u | d); }  // movi vd.2d, #0
    // zip1 vd, vn, vm (size: 1 = 8h, 2 = 4s, 3 = 2d)
    void zip1(unsigned d, unsigned n, unsigned m, unsigned size) { w(0x4E003800u | size << 22 | m << 16 | n << 5 | d); }
    void add_xi(unsigned d, unsigned n, std::uint32_t imm12) { w(0x91000000u | imm12 << 10 | n << 5 | d); }
    // control
    void blr(unsigned n) { w(0xD63F0000u | n << 5); }
    void ret() { w(0xD65F03C0u); }
    std::uint32_t *cbz_w(unsigned t) { w(0x34000000u | t); return p_ - 1; }
    std::uint32_t *cbz_x(unsigned t) { w(0xB4000000u | t); return p_ - 1; }
    std::uint32_t *cbnz_w(unsigned t) { w(0x35000000u | t); return p_ - 1; }
    std::uint32_t *b() { w(0x14000000u); return p_ - 1; }
    void patch19(std::uint32_t *at) { *at |= (static_cast<std::uint32_t>(p_ - at) & 0x7FFFFu) << 5; }
    void patch26(std::uint32_t *at) { *at |= static_cast<std::uint32_t>(p_ - at) & 0x3FFFFFFu; }
    void call(const void *fn) {
        imm64(X16, reinterpret_cast<std::uint64_t>(fn));
        blr(X16);
    }

private:
    std::uint32_t *start_, *p_;
};

// [x20, wm, uxtw] forms
constexpr std::uint32_t LDRB = 0x38604800u, LDRH = 0x78604800u, LDRW = 0xB8604800u, LDRX = 0xF8604800u;
constexpr std::uint32_t STRB = 0x38204800u, STRH = 0x78204800u, STRW = 0xB8204800u, STRX = 0xF8204800u;

struct Block {
    // read by chained jumps from generated code: keep these three first
    std::uint32_t pc = 0;
    std::uint32_t checked = 0;   // code epoch the guest words were last proven in
    const void *body = nullptr;  // entry after the prologue (chained jumps)
    std::uint32_t words = 0;
    std::uint64_t (*fn)(Cpu *) = nullptr;
    std::unique_ptr<std::uint32_t[]> code;    // guest words, checked on entry (self-modifying code)
    std::unique_ptr<Insn[]> insns;           // decoded instructions the helpers get pointers to
};

// block table, direct mapped by pc (8 MB of code without conflicts); chained jumps read it too
constexpr std::uint32_t kTable = 1u << 21;
// PS2_EE_JIT_FUSE=0: no fused unaligned pairs (A/B check)
const bool g_fuse = [] {
    const char *v = std::getenv("PS2_EE_JIT_FUSE");
    return !v || std::strcmp(v, "0") != 0;
}();
Block *g_table[kTable];

class Compiler {
public:
    Compiler(Asm &a, Block &b) : a_(a), b_(b) {}

    void prologue() {
        a_.w(0xA9BD7BFDu);  // stp x29, x30, [sp, #-48]!
        a_.w(0x910003FDu);  // mov x29, sp
        a_.stp_x(19, 20, 31, 16);
        a_.stp_x(21, 22, 31, 32);
        a_.mov_x(C, 0);
        a_.imm64(RAM, reinterpret_cast<std::uint64_t>(g_ram));
        b_.body = a_.here();
    }

    // Leaves with c.pc = pc (or w21 when from_t) and returns the packed exit. A next/back exit to a
    // constant target first tries to continue straight in the target's translated block.
    void leave(std::uint64_t exit, std::uint32_t pc, bool from_t = false) {
        if (from_t) a_.str_w(T, C, kOffPc);
        else {
            a_.imm32(0, pc);
            a_.str_w(0, C, kOffPc);
            const Exit e = static_cast<Exit>(exit & 0xFFu);
            if (e == kNext || e == kBack) chain(e, pc);
        }
        a_.imm64(0, exit);
        epilogue();
    }
    // w0 = target. Jumps to the target block's body when it is in the table, for this pc, proven in
    // the current code epoch, and (back jumps) the budget is not used up; else falls through.
    void chain(Exit e, std::uint32_t target) {
        if (target >= kRamSize - 4u * (kMaxBlock + 2) || (target & 3u) || probe_at(target)) return;
        std::uint32_t *outs[4];
        int n = 0;
        if (e == kBack) {
            a_.ldr_w(1, C, kOffBudget);
            a_.cmp_w(1, ZR);
            outs[n++] = a_.bcond(LT);
        }
        a_.imm64(1, reinterpret_cast<std::uint64_t>(&g_table[(target >> 2) & (kTable - 1)]));
        a_.ldr_x(1, 1, 0);
        outs[n++] = a_.cbz_x(1);
        a_.ldr_w(2, 1, offsetof(Block, pc));
        a_.cmp_w(2, 0);
        outs[n++] = a_.bcond(NE);
        a_.ldr_w(2, 1, offsetof(Block, checked));
        a_.imm64(3, reinterpret_cast<std::uint64_t>(&g_code_epoch));
        a_.ldr_w(3, 3, 0);
        a_.cmp_w(2, 3);
        outs[n++] = a_.bcond(NE);
        a_.ldr_x(2, 1, offsetof(Block, body));
        a_.br(2);
        for (int k = 0; k < n; ++k) a_.patch19(outs[k]);
    }
    void fp2(std::uint32_t dst_off, unsigned fs, unsigned ft, const void *fn) {
        a_.ldr_w(0, C, fr(fs));
        a_.ldr_w(1, C, fr(ft));
        a_.call(fn);
        a_.str_w(0, C, dst_off);
    }
    void fp3(std::uint32_t dst_off, unsigned fs, unsigned ft, const void *fn) {
        a_.ldr_w(0, C, kOffFacc);
        a_.ldr_w(1, C, fr(fs));
        a_.ldr_w(2, C, fr(ft));
        a_.call(fn);
        a_.str_w(0, C, dst_off);
    }
    void fp1(std::uint32_t dst_off, unsigned src, const void *fn) {
        a_.ldr_w(0, C, fr(src));
        a_.call(fn);
        a_.str_w(0, C, dst_off);
    }
    void leave_keep_pc(std::uint64_t exit) {
        a_.imm64(0, exit);
        epilogue();
    }
    void epilogue() {
        a_.ldp_x(21, 22, 31, 32);
        a_.ldp_x(19, 20, 31, 16);
        a_.w(0xA8C37BFDu);  // ldp x29, x30, [sp], #48
        a_.ret();
    }
    void halt_if_zero() {  // after ee_h_simple: w0 == 0 -> stopped
        std::uint32_t *ok = a_.cbnz_w(0);
        leave_keep_pc(pack(kHalt));
        a_.patch19(ok);
    }
    void charge(std::int32_t n) {
        if (n <= 0) return;
        a_.ldr_w(0, C, kOffBudget);
        a_.sub_wi(0, 0, static_cast<std::uint32_t>(n));
        a_.str_w(0, C, kOffBudget);
    }

    // guest register access
    unsigned src(unsigned r, unsigned tmp) {  // 64-bit value in a host register (ZR for $zero)
        if (!r) return ZR;
        a_.ldr_x(tmp, C, gr(r));
        return tmp;
    }
    void put(unsigned r, unsigned host) { if (r) a_.str_x(host, C, gr(r)); }
    void put32(unsigned r, unsigned host) {  // sign-extend the low word
        if (!r) return;
        a_.sxtw(host, host);
        a_.str_x(host, C, gr(r));
    }
    // wd = guest rs + imm (32-bit). The immediate add forms read register 31 as SP, so $zero
    // becomes a plain constant.
    void add_imm_w(unsigned d, unsigned rs, std::int32_t imm) {
        if (!rs) {
            a_.imm32(d, static_cast<std::uint32_t>(imm));
            return;
        }
        a_.ldr_w(d, C, gr(rs));
        if (imm > 0 && imm < 4096) a_.add_wi(d, d, static_cast<std::uint32_t>(imm));
        else if (imm < 0 && imm > -4096) a_.sub_wi(d, d, static_cast<std::uint32_t>(-imm));
        else if (imm) {
            a_.imm32(1, static_cast<std::uint32_t>(imm));
            a_.add_w(d, d, 1);
        }
    }
    void address(const Insn &i) { add_imm_w(0, i.rs(), i.simm()); }  // w0 = rs + simm
    std::uint32_t *slow_if_not_ram() {  // w0 = address; branch out when >= 32 MB
        a_.ubfm_w(1, 0, 25, 31);  // lsr w1, w0, #25
        return a_.cbnz_w(1);
    }

    // load: x2 = value (zero-extended), then extend/store
    void load(const Insn &i, std::uint32_t op, const void *helper, int extend /*0 none, 8/16/32 signed*/, bool fpr = false) {
        address(i);
        std::uint32_t *slow = slow_if_not_ram();
        a_.ram(op, 2, 0);
        std::uint32_t *done = a_.b();
        a_.patch19(slow);
        a_.call(helper);
        a_.mov_x(2, 0);
        a_.patch26(done);
        if (fpr) {
            a_.str_w(2, C, kOffF + i.ft() * 4u);
            return;
        }
        if (!i.rt()) return;
        if (extend == 8) a_.sxtb(2, 2);
        if (extend == 16) a_.sxth(2, 2);
        if (extend == 32) a_.sxtw(2, 2);
        a_.str_x(2, C, gr(i.rt()));
    }
    void store(const Insn &i, std::uint32_t op, const void *helper, bool fpr = false) {
        address(i);
        unsigned v;
        if (fpr) {
            a_.ldr_w(1, C, kOffF + i.ft() * 4u);
            v = 1;
        } else {
            v = src(i.rt(), 1);
        }
        a_.ubfm_w(3, 0, 25, 31);
        std::uint32_t *slow = a_.cbnz_w(3);
        a_.ram(op, v, 0);
        std::uint32_t *done = a_.b();
        a_.patch19(slow);
        if (v != 1) a_.mov_x(1, v);
        a_.call(helper);
        a_.patch26(done);
    }
    void quad(const Insn &i, bool is_store, std::uint32_t reg_off, bool discard) {
        address(i);
        a_.and_w_not15(0, 0);
        std::uint32_t *slow = slow_if_not_ram();
        a_.add_x(2, RAM, 0);  // x2 = g_ram + w0 (w0 < 32 MB, upper bits clear)
        if (is_store) {
            a_.ldr_q(0, C, reg_off);
            a_.str_q(0, 2, 0);
        } else if (!discard) {
            a_.ldr_q(0, 2, 0);
            a_.str_q(0, C, reg_off);
        }
        std::uint32_t *done = a_.b();
        a_.patch19(slow);
        if (is_store) {
            a_.imm64(1, reg_off);
            a_.add_x(1, C, 1);
            a_.call(reinterpret_cast<const void *>(&ee_h_st128));
        } else {
            // into the guest register, or a scratch slot below the frame for lq $zero
            if (discard) {
                a_.w(0xD10043FFu);  // sub sp, sp, #16
                a_.w(0x910003E1u);  // mov x1, sp
            } else {
                a_.imm64(1, reg_off);
                a_.add_x(1, C, 1);
            }
            a_.call(reinterpret_cast<const void *>(&ee_h_ld128));
            if (discard) a_.w(0x910043FFu);  // add sp, sp, #16
        }
        a_.patch26(done);
    }

#define FPH(name) reinterpret_cast<const void *>(&name)
    // An unaligned pair at the same base (ldl rt, k+7 / ldr rt, k in either order; lwl/lwr k+3 / k;
    // the stores alike): one unaligned access when the bytes lie in RAM, else both helpers in order.
    // Same result as the two instructions: they assemble exactly the bytes at k (loads: lwl/lwr leave
    // the word sign-extended either way; ldl/ldr the full doubleword; the upper 64 bits untouched).
    bool fused_pair(const Insn &a, const Insn &b) {
        const bool is64 = a.op == Ldl || a.op == Ldr || a.op == Sdl || a.op == Sdr;
        recomp::Op lo, hi;
        switch (a.op) {
        case Ldl: case Ldr: lo = Ldr; hi = Ldl; break;
        case Lwl: case Lwr: lo = Lwr; hi = Lwl; break;
        case Sdl: case Sdr: lo = Sdr; hi = Sdl; break;
        case Swl: case Swr: lo = Swr; hi = Swl; break;
        default: return false;
        }
        if (!((a.op == lo && b.op == hi) || (a.op == hi && b.op == lo))) return false;
        if (a.rs() != b.rs() || a.rt() != b.rt()) return false;
        const bool load = lo == Ldr || lo == Lwr;
        if (load && (a.rt() == a.rs() || !a.rt())) return false;  // base changed by the first / no target
        const Insn &l = a.op == lo ? a : b, &h = a.op == lo ? b : a;
        const std::int32_t width = is64 ? 8 : 4;
        if (h.simm() != l.simm() + width - 1) return false;
        add_imm_w(0, l.rs(), l.simm());  // w0 = k
        a_.mov_x(5, 0);
        // fast path: k .. k+width-1 in RAM
        a_.imm32(1, kRamSize - static_cast<std::uint32_t>(width));
        a_.cmp_w(0, 1);
        std::uint32_t *slow = a_.bcond(HS);
        const unsigned rt = l.rt();
        if (load) {
            a_.ram(is64 ? LDRX : LDRW, 2, 0);
            if (!is64) a_.sxtw(2, 2);
            a_.str_x(2, C, gr(rt));
        } else {
            a_.ram(is64 ? STRX : STRW, src(rt, 2), 0);
        }
        std::uint32_t *done = a_.b();
        a_.patch19(slow);
        const auto helper = [&](recomp::Op op) -> const void * {
            switch (op) {
            case Ldl: return FPH(ee_x_ldl);
            case Ldr: return FPH(ee_x_ldr);
            case Lwl: return FPH(ee_x_lwl);
            case Lwr: return FPH(ee_x_lwr);
            case Sdl: return FPH(ee_x_sdl);
            case Sdr: return FPH(ee_x_sdr);
            case Swl: return FPH(ee_x_swl);
            default: return FPH(ee_x_swr);
            }
        };
        for (const Insn *x : {&a, &b}) {
            a_.add_xi(0, C, gr(rt));
            if (x->op == lo) a_.mov_x(1, 5);
            else a_.add_wi(1, 5, static_cast<std::uint32_t>(width - 1));
            a_.call(helper(x->op));
            if (x == &a) {  // x5 is caller-saved: rebuild k for the second call
                add_imm_w(0, l.rs(), l.simm());
                a_.mov_x(5, 0);
            }
        }
        a_.patch26(done);
        return true;
    }

    // One non-control instruction: native when simple, else the interpreter's simple().
    void op(const Insn &i, const Insn *stored) {
        const unsigned rs = i.rs(), rt = i.rt(), rd = i.rd(), sa = i.sa();
        switch (i.op) {
        case Sll:
            if (!i.word || !rd) return;
            a_.ldr_w(0, C, gr(rt));
            a_.ubfm_w(0, 0, (32u - sa) & 31u, 31u - sa);
            put32(rd, 0);
            return;
        case Srl:
            if (!rd) return;
            a_.ldr_w(0, C, gr(rt));
            a_.ubfm_w(0, 0, sa, 31);
            put32(rd, 0);
            return;
        case Sra:
            if (!rd) return;
            a_.ldr_w(0, C, gr(rt));
            a_.sbfm_w(0, 0, sa, 31);
            put32(rd, 0);
            return;
        case Sllv: case Srlv: case Srav: {
            if (!rd) return;
            const unsigned t = src(rt, 0), s = src(rs, 1);
            if (i.op == Sllv) a_.lslv_w(0, t, s);
            else if (i.op == Srlv) a_.lsrv_w(0, t, s);
            else a_.asrv_w(0, t, s);
            put32(rd, 0);
            return;
        }
        case Dsllv: case Dsrlv: case Dsrav: {
            if (!rd) return;
            const unsigned t = src(rt, 0), s = src(rs, 1);
            if (i.op == Dsllv) a_.lslv_x(0, t, s);
            else if (i.op == Dsrlv) a_.lsrv_x(0, t, s);
            else a_.asrv_x(0, t, s);
            put(rd, 0);
            return;
        }
        case Dsll: case Dsll32: case Dsrl: case Dsrl32: case Dsra: case Dsra32: {
            if (!rd) return;
            const unsigned sh = sa + (i.op == Dsll32 || i.op == Dsrl32 || i.op == Dsra32 ? 32u : 0u);
            const unsigned t = src(rt, 0);
            if (i.op == Dsll || i.op == Dsll32) a_.ubfm_x(0, t, (64u - sh) & 63u, 63u - sh);
            else if (i.op == Dsrl || i.op == Dsrl32) a_.ubfm_x(0, t, sh, 63);
            else a_.sbfm_x(0, t, sh, 63);
            put(rd, 0);
            return;
        }
        case Movz: case Movn: {
            if (!rd) return;
            const unsigned t = src(rt, 0), s = src(rs, 1);
            a_.ldr_x(2, C, gr(rd));
            a_.cmp_x(t, ZR);
            a_.csel_x(2, s, 2, i.op == Movz ? EQ : NE);
            put(rd, 2);
            return;
        }
        case Mfhi: if (rd) { a_.ldr_x(0, C, kOffHi); put(rd, 0); } return;
        case Mflo: if (rd) { a_.ldr_x(0, C, kOffLo); put(rd, 0); } return;
        case Mthi: a_.str_x(src(rs, 0), C, kOffHi); return;
        case Mtlo: a_.str_x(src(rs, 0), C, kOffLo); return;
        case Add: case Addu: case Sub: case Subu: {
            if (!rd) return;
            const unsigned s = src(rs, 0), t = src(rt, 1);
            if (i.op == Add || i.op == Addu) a_.add_w(0, s, t);
            else a_.sub_w(0, s, t);
            put32(rd, 0);
            return;
        }
        case Dadd: case Daddu: case Dsub: case Dsubu: case And: case Or: case Xor: case Nor: {
            if (!rd) return;
            const unsigned s = src(rs, 0), t = src(rt, 1);
            switch (i.op) {
            case Dadd: case Daddu: a_.add_x(0, s, t); break;
            case Dsub: case Dsubu: a_.sub_x(0, s, t); break;
            case And: a_.and_x(0, s, t); break;
            case Or: a_.orr_x(0, s, t); break;
            case Xor: a_.eor_x(0, s, t); break;
            default: a_.orr_x(0, s, t); a_.orn_x(0, ZR, 0); break;
            }
            put(rd, 0);
            return;
        }
        case Slt: case Sltu: {
            if (!rd) return;
            a_.cmp_x(src(rs, 0), src(rt, 1));
            a_.cset_w(0, i.op == Slt ? LT : LO);
            put(rd, 0);
            return;
        }
        case Slti: case Sltiu: {
            if (!rt) return;
            a_.imm64(1, static_cast<std::uint64_t>(static_cast<std::int64_t>(i.simm())));
            a_.cmp_x(src(rs, 0), 1);
            a_.cset_w(0, i.op == Slti ? LT : LO);
            put(rt, 0);
            return;
        }
        case Addi: case Addiu: {
            if (!rt) return;
            add_imm_w(0, rs, i.simm());
            put32(rt, 0);
            return;
        }
        case Daddi: case Daddiu: {
            if (!rt) return;
            a_.imm64(1, static_cast<std::uint64_t>(static_cast<std::int64_t>(i.simm())));
            a_.add_x(0, src(rs, 0), 1);
            put(rt, 0);
            return;
        }
        case Andi: case Ori: case Xori: {
            if (!rt) return;
            a_.imm32(1, i.uimm());
            const unsigned s = src(rs, 0);
            if (i.op == Andi) a_.and_x(0, s, 1);
            else if (i.op == Ori) a_.orr_x(0, s, 1);
            else a_.eor_x(0, s, 1);
            put(rt, 0);
            return;
        }
        case Lui:
            if (!rt) return;
            a_.imm32(0, i.uimm() << 16);
            put32(rt, 0);
            return;
        case Lb: load(i, LDRB, reinterpret_cast<const void *>(&ee_h_ld8), 8); return;
        case Lbu: load(i, LDRB, reinterpret_cast<const void *>(&ee_h_ld8), 0); return;
        case Lh: load(i, LDRH, reinterpret_cast<const void *>(&ee_h_ld16), 16); return;
        case Lhu: load(i, LDRH, reinterpret_cast<const void *>(&ee_h_ld16), 0); return;
        case Lw: load(i, LDRW, reinterpret_cast<const void *>(&ee_h_ld32), 32); return;
        case Lwu: load(i, LDRW, reinterpret_cast<const void *>(&ee_h_ld32), 0); return;
        case Ld: load(i, LDRX, reinterpret_cast<const void *>(&ee_h_ld64), 0); return;
        case Lwc1: load(i, LDRW, reinterpret_cast<const void *>(&ee_h_ld32), 0, true); return;
        case Sb: store(i, STRB, reinterpret_cast<const void *>(&ee_h_st8)); return;
        case Sh: store(i, STRH, reinterpret_cast<const void *>(&ee_h_st16)); return;
        case Sw: store(i, STRW, reinterpret_cast<const void *>(&ee_h_st32)); return;
        case Sd: store(i, STRX, reinterpret_cast<const void *>(&ee_h_st64)); return;
        case Swc1: store(i, STRW, reinterpret_cast<const void *>(&ee_h_st32), true); return;
        case Lq: quad(i, false, gr(rt), rt == 0); return;
        case Sq: quad(i, true, gr(rt), false); return;
        case Lqc2: quad(i, false, vfr(i.ft()), i.ft() == 0); return;
        case Sqc2: quad(i, true, vfr(i.ft()), false); return;
        case Vnop: case Vwaitq: return;
        case Qmfc2:
            if (i.word & 1u) break;  // interlock: vu0_wait in simple()
            if (rt) {
                a_.ldr_q(0, C, vfr(i.fs()));
                a_.str_q(0, C, gr(rt));
            }
            return;
        case Qmtc2:
            if (i.word & 1u) break;
            if (i.fs()) {
                if (rt) a_.ldr_q(0, C, gr(rt));
                else a_.movi_zero_q(0);
                a_.str_q(0, C, vfr(i.fs()));
            }
            return;
        case Cfc2:
            if (i.fs() >= 16) break;
            if (rt) {
                a_.ldrh_w(0, C, vir(i.fs()));
                a_.str_x(0, C, gr(rt));  // < 0x10000: set32's sign extension keeps it
            }
            return;
        case Ctc2:
            if (i.fs() >= 16) break;
            if (i.fs()) a_.strh_w(src(rt, 0), C, vir(i.fs()));
            return;
        case Vmove:
            if (!i.ft()) return;
            for (unsigned k = 0; k < 4; ++k)
                if (i.dest() & (8u >> k)) {
                    a_.ldr_w(0, C, vfr(i.fs()) + 4u * k);
                    a_.str_w(0, C, vfr(i.ft()) + 4u * k);
                }
            return;
        case Pcpyld: case Pextlw: case Pextlh: {
            if (!rd) return;
            if (rt) a_.ldr_q(0, C, gr(rt));
            else a_.movi_zero_q(0);
            if (rs) a_.ldr_q(1, C, gr(rs));
            else a_.movi_zero_q(1);
            a_.zip1(2, 0, 1, i.op == Pcpyld ? 3u : i.op == Pextlw ? 2u : 1u);
            a_.str_q(2, C, gr(rd));
            return;
        }
        case Ldl: case Ldr: case Lwl: case Lwr:
            if (!rt) return;
            address(i);
            a_.mov_x(1, 0);
            a_.add_xi(0, C, gr(rt));
            a_.call(i.op == Ldl ? FPH(ee_x_ldl) : i.op == Ldr ? FPH(ee_x_ldr) : i.op == Lwl ? FPH(ee_x_lwl) : FPH(ee_x_lwr));
            return;
        case Sdl: case Sdr: case Swl: case Swr:
            address(i);
            a_.mov_x(1, 0);
            a_.add_xi(0, C, gr(rt));
            a_.call(i.op == Sdl ? FPH(ee_x_sdl) : i.op == Sdr ? FPH(ee_x_sdr) : i.op == Swl ? FPH(ee_x_swl) : FPH(ee_x_swr));
            return;
        case DivS:
            a_.mov_x(0, C);
            a_.imm32(1, i.fd());
            a_.ldr_w(2, C, fr(i.fs()));
            a_.ldr_w(3, C, fr(i.ft()));
            a_.call(FPH(ee_x_divs));
            return;
        case Vmaddabc: case Vmulabc: case Vmaddbc: case Vmulbc: case Vaddbc: case Vsubbc: case Vmul: case Vadd: case Vsub:
        case Vmadd: case Vmula: case Vmadda: case Vclip: {
            const void *fn = nullptr;
            switch (i.op) {
            case Vmaddabc: fn = FPH(ee_v_maddabc); break;
            case Vmulabc: fn = FPH(ee_v_mulabc); break;
            case Vmaddbc: fn = FPH(ee_v_maddbc); break;
            case Vmulbc: fn = FPH(ee_v_mulbc); break;
            case Vaddbc: fn = FPH(ee_v_addbc); break;
            case Vsubbc: fn = FPH(ee_v_subbc); break;
            case Vmul: fn = FPH(ee_v_mul); break;
            case Vadd: fn = FPH(ee_v_add); break;
            case Vsub: fn = FPH(ee_v_sub); break;
            case Vmadd: fn = FPH(ee_v_madd); break;
            case Vmula: fn = FPH(ee_v_mula); break;
            case Vmadda: fn = FPH(ee_v_madda); break;
            default: fn = FPH(ee_v_clip); break;
            }
            a_.add_xi(0, C, kOffVu0);
            a_.imm64(1, reinterpret_cast<std::uint64_t>(stored));
            a_.call(fn);
            return;
        }
        case Mfc1: if (rt) { a_.ldr_w(0, C, kOffF + i.fs() * 4u); put32(rt, 0); } return;
        case Mtc1: a_.str_w(src(rt, 0), C, kOffF + i.fs() * 4u); return;
        case MovS: a_.ldr_w(0, C, kOffF + i.fs() * 4u); a_.str_w(0, C, kOffF + i.fd() * 4u); return;
        case Mult: case Multu: {
            const unsigned s = src(rs, 0), t = src(rt, 1);
            if (i.op == Mult) a_.smull(2, s, t);
            else a_.umull(2, s, t);
            a_.sxtw(3, 2);
            a_.sbfm_x(4, 2, 32, 63);
            a_.str_x(3, C, kOffLo);
            a_.str_x(4, C, kOffHi);
            put(rd, 3);
            return;
        }
#define FPF(name) reinterpret_cast<const void *>(&name)
        case AddS: fp2(fr(i.fd()), i.fs(), i.ft(), FPF(ee_fp_add)); return;
        case SubS: fp2(fr(i.fd()), i.fs(), i.ft(), FPF(ee_fp_sub)); return;
        case MulS: fp2(fr(i.fd()), i.fs(), i.ft(), FPF(ee_fp_mul)); return;
        case MaxS: fp2(fr(i.fd()), i.fs(), i.ft(), FPF(ee_fp_max)); return;
        case MinS: fp2(fr(i.fd()), i.fs(), i.ft(), FPF(ee_fp_min)); return;
        case AddaS: fp2(kOffFacc, i.fs(), i.ft(), FPF(ee_fp_add)); return;
        case SubaS: fp2(kOffFacc, i.fs(), i.ft(), FPF(ee_fp_sub)); return;
        case MulaS: fp2(kOffFacc, i.fs(), i.ft(), FPF(ee_fp_mul)); return;
        case MaddS: fp3(fr(i.fd()), i.fs(), i.ft(), FPF(ee_fp_madd)); return;
        case MsubS: fp3(fr(i.fd()), i.fs(), i.ft(), FPF(ee_fp_msub)); return;
        case MaddaS: fp3(kOffFacc, i.fs(), i.ft(), FPF(ee_fp_madd)); return;
        case MsubaS: fp3(kOffFacc, i.fs(), i.ft(), FPF(ee_fp_msub)); return;
        case CvtWS: fp1(fr(i.fd()), i.fs(), FPF(ee_fp_cvtws)); return;
        case CvtSW: fp1(fr(i.fd()), i.fs(), FPF(ee_fp_cvtsw)); return;
        case SqrtS: fp1(fr(i.fd()), i.ft(), FPF(ee_fp_sqrt)); return;
        case AbsS: a_.ldr_w(0, C, fr(i.fs())); a_.and_w_abs(0, 0); a_.str_w(0, C, fr(i.fd())); return;
        case NegS: a_.ldr_w(0, C, fr(i.fs())); a_.eor_w_sign(0, 0); a_.str_w(0, C, fr(i.fd())); return;
        case CEq: case CLt: case CLe:
            a_.mov_x(0, C);
            a_.ldr_w(1, C, fr(i.fs()));
            a_.ldr_w(2, C, fr(i.ft()));
            a_.imm32(3, i.op == CEq ? 0 : i.op == CLt ? 1 : 2);
            a_.call(FPF(ee_fp_cmp));
            return;
        case Cache: a_.call(FPF(ee_h_cache)); return;
#undef FPF
        case Sync: case Pref: return;
        default: break;
        }
        // everything else (and the cases above that break out): the interpreter's simple()
        a_.mov_x(0, C);
        a_.imm64(1, reinterpret_cast<std::uint64_t>(stored));
        a_.call(reinterpret_cast<const void *>(&ee_h_simple));
        halt_if_zero();
    }

    // w21 = branch condition
    void cond(const Insn &i, const Insn *stored) {
        switch (i.op) {
        case Beq: case Beql: case Bne: case Bnel:
            a_.cmp_x(src(i.rs(), 0), src(i.rt(), 1));
            a_.cset_w(T, i.op == Beq || i.op == Beql ? EQ : NE);
            return;
        case Blez: case Blezl: a_.cmp_x(src(i.rs(), 0), ZR); a_.cset_w(T, LE); return;
        case Bgtz: case Bgtzl: a_.cmp_x(src(i.rs(), 0), ZR); a_.cset_w(T, GT); return;
        case Bltz: case Bltzl: case Bltzal: case Bltzall: a_.cmp_x(src(i.rs(), 0), ZR); a_.cset_w(T, LT); return;
        case Bgez: case Bgezl: case Bgezal: case Bgezall: a_.cmp_x(src(i.rs(), 0), ZR); a_.cset_w(T, GE); return;
        case Bc1f: case Bc1fl: case Bc1t: case Bc1tl:
            a_.ldr_w(0, C, kOffFcr31);
            a_.ubfm_w(T, 0, 23, 23);
            if (i.op == Bc1f || i.op == Bc1fl) a_.eor_w1(T, T);
            return;
        default:  // bc0x / bc2x
            a_.mov_x(0, C);
            a_.imm64(1, reinterpret_cast<std::uint64_t>(stored));
            a_.call(reinterpret_cast<const void *>(&ee_h_cond));
            a_.mov_x(T, 0);
            return;
        }
    }

    // Calls the function at `target` (or w21) in place; continues at ret when it returned there,
    // else leaves this frame like generated code's `if (c.pc != ret) return;`.
    void call_in_place(std::uint32_t target, std::uint32_t ret, bool from_t = false) {
        a_.mov_x(0, C);
        if (from_t) a_.mov_x(1, T);
        else a_.imm32(1, target);
        a_.imm32(2, ret);
        a_.call(reinterpret_cast<const void *>(&ee_h_call));
        std::uint32_t *ok = a_.cbnz_w(0);
        leave_keep_pc(pack(kReturn));
        a_.patch19(ok);
        leave(pack(kNext), ret);
    }
    void link(std::uint32_t reg, std::uint32_t ret) {
        a_.imm64(0, static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(ret))));
        a_.str_x(0, C, gr(reg));
    }
    void stop_here(std::uint32_t pc, const char *what) {
        a_.mov_x(0, C);
        a_.imm32(1, pc);
        a_.imm64(2, reinterpret_cast<std::uint64_t>(what));
        a_.call(reinterpret_cast<const void *>(&ee_h_stop));
        leave_keep_pc(pack(kHalt));
    }

    // Emits the whole block starting at b_.pc (rules of interp_block).
    void block() {
        prologue();
        std::vector<Insn> list;
        std::uint32_t pc = b_.pc;
        // scan first: the block's words are needed for the entry check
        for (std::uint32_t n = 0;; ++n, pc += 4) {
            if (n && (n >= kMaxBlock || probe_at(pc))) break;
            const Insn i = recomp::decode(ld32(pc), pc);
            list.push_back(i);
            const Flow fl = recomp::flow(i.op);
            if (fl == Flow::None && i.op != Syscall) continue;
            if (fl != Flow::None && fl != Flow::Stop) list.push_back(recomp::decode(ld32(pc + 4), pc + 4));
            break;
        }
        b_.words = static_cast<std::uint32_t>(list.size());
        b_.code = std::make_unique<std::uint32_t[]>(list.size());
        b_.insns = std::make_unique<Insn[]>(list.size());
        for (std::size_t k = 0; k < list.size(); ++k) {
            b_.code[k] = list[k].word;
            b_.insns[k] = list[k];
        }

        std::int32_t cost = 0;
        bool fused = false;  // this instruction was emitted with the previous one (fused_pair)
        for (std::size_t k = 0; k < list.size(); ++k) {
            const Insn &i = b_.insns[k];
            const std::uint32_t at = i.pc;
            const Flow fl = recomp::flow(i.op);
            if (fl == Flow::None) {
                ++cost;
                if (i.op == Syscall) {
                    charge(cost);
                    leave(pack(kSyscall, at), at + 4);
                    return;
                }
                if (fused) {
                    fused = false;
                } else if (k + 1 < list.size() && recomp::flow(b_.insns[k + 1].op) == Flow::None &&
                           b_.insns[k + 1].op != Syscall && g_fuse && fused_pair(i, b_.insns[k + 1])) {
                    fused = true;
                } else {
                    op(i, &b_.insns[k]);
                }
                if (k + 1 == list.size()) {  // block cut (size limit or probe)
                    charge(cost);
                    leave(pack(kNext), at + 4);
                    return;
                }
                continue;
            }
            if (i.op == Eret) {
                charge(cost);
                leave_keep_pc(pack(kEret));
                return;
            }
            if (fl == Flow::Stop) {
                stop_here(at, recomp::name(i.op));
                return;
            }
            charge(cost + 2);
            const Insn &ds = b_.insns[k + 1];
            if (ds.op == Invalid || recomp::has_delay_slot(ds.op) || ds.op == Syscall) {
                stop_here(at + 4, "unsupported delay slot");
                return;
            }
            control(i, &b_.insns[k], ds, &b_.insns[k + 1]);
            return;
        }
    }

    void control(const Insn &i, const Insn *si, const Insn &ds, const Insn *sds) {
        const std::uint32_t pc = i.pc, ret = pc + 8, t = i.branch_target_of();
        const auto taken_exit = [&](std::uint32_t target) { return pack(target <= pc ? kBack : kNext); };
        switch (i.op) {
        case Jal:
            link(31, ret);
            op(ds, sds);
            call_in_place(t, ret);
            return;
        case Bltzal: case Bgezal: {
            cond(i, si);
            link(31, ret);
            op(ds, sds);
            std::uint32_t *no = a_.cbz_w(T);
            call_in_place(t, ret);
            a_.patch19(no);
            leave(pack(kNext), ret);
            return;
        }
        case Bltzall: case Bgezall: {
            cond(i, si);
            link(31, ret);
            std::uint32_t *no = a_.cbz_w(T);
            op(ds, sds);
            call_in_place(t, ret);
            a_.patch19(no);
            leave(pack(kNext), ret);
            return;
        }
        case Jalr:
            a_.ldr_w(T, C, gr(i.rs()));
            if (!i.rs()) a_.imm32(T, 0);
            if (i.rd()) link(i.rd(), ret);
            op(ds, sds);
            call_in_place(0, ret, true);
            return;
        case Jr:
            a_.ldr_w(T, C, gr(i.rs()));
            if (!i.rs()) a_.imm32(T, 0);
            op(ds, sds);
            leave(pack(i.rs() == 31 ? kReturn : kJr), 0, true);
            return;
        case J:
            op(ds, sds);
            leave(taken_exit(t), t);
            return;
        default:
            break;
        }
        const Flow fl = recomp::flow(i.op);
        cond(i, si);
        if (fl == Flow::Branch) {
            op(ds, sds);
            std::uint32_t *no = a_.cbz_w(T);
            leave(taken_exit(t), t);
            a_.patch19(no);
            leave(pack(kNext), ret);
        } else {  // likely: the delay slot runs only when taken
            std::uint32_t *no = a_.cbz_w(T);
            op(ds, sds);
            leave(taken_exit(t), t);
            a_.patch19(no);
            leave(pack(kNext), ret);
        }
    }

private:
    Asm &a_;
    Block &b_;
};

// ---- block cache: g_table (above), backed by a map so conflicting blocks aren't recompiled
std::unordered_map<std::uint32_t, std::unique_ptr<Block>> g_blocks;
std::deque<std::unique_ptr<Block>> g_retired;  // replaced blocks may still be on the stack
std::uint64_t g_compiled = 0;

Block *compile(std::uint32_t pc) {
    std::uint32_t *mem = reserve(kMaxBlock * 64u * 4u + 4096u);
    if (!mem) return nullptr;
    auto b = std::make_unique<Block>();
    b->pc = pc;
    Asm a(mem);
    Compiler(a, *b).block();
    __builtin___clear_cache(reinterpret_cast<char *>(a.start()), reinterpret_cast<char *>(a.here()));
    g_chunks.back().used += (a.bytes() + 15u) & ~std::size_t{15};
    b->fn = reinterpret_cast<std::uint64_t (*)(Cpu *)>(a.start());
    ++g_compiled;
    auto &slot = g_blocks[pc];
    if (slot) g_retired.push_back(std::move(slot));
    slot = std::move(b);
    return slot.get();
}

inline bool current(const Block *b) {
    return std::memcmp(g_ram + b->pc, b->code.get(), b->words * 4u) == 0;
}

}

bool jit_enabled() {
    const char *e = std::getenv("PS2_EE_JIT");
    if (e && e[0] == '0') return false;
    if (g_watch_on) return false;  // the JIT's fast stores skip the debug watch
    static bool reported = false;
    if (!reported) {
        reported = true;
        std::fprintf(stderr, "[ee-jit] ARM64 JIT on\n");
    }
    return true;
}

void jit_flush() {
    std::memset(g_table, 0, sizeof g_table);
    for (auto &[pc, b] : g_blocks) g_retired.push_back(std::move(b));
    g_blocks.clear();
}

std::uint64_t jit_block(Cpu &c) {
    const std::uint32_t pc = c.pc;
    if (pc >= kRamSize - 4u * (kMaxBlock + 2) || (pc & 3u)) return interp_block(c);
    Block *&slot = g_table[(pc >> 2) & (kTable - 1)];
    Block *b = slot;
    if (!b || b->pc != pc || b->checked != g_code_epoch) [[unlikely]] {
        if (!b || b->pc != pc) {
            const auto it = g_blocks.find(pc);
            b = it != g_blocks.end() ? it->second.get() : nullptr;
        }
        if (!b || !current(b)) b = compile(pc);
        if (!b) return interp_block(c);
        b->checked = g_code_epoch;
        slot = b;
    }
    return b->fn(&c);
}

}

#endif

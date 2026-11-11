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
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

namespace ps2::vu::jit {

namespace {

vuops::FlagPipe g_fl;  // the flag pipe the translated programs use (Layout::flags)
std::uintptr_t g_base = 0;
bool g_base_ok = false;
std::uint64_t kParamOffset[2] = {};
static_assert(sizeof(vuops::FlagPipe::mh) == 4 && sizeof(vuops::FlagPipe::t) == 4 && offsetof(vuops::FlagPipe, clip_v) + 32 < 16384,
              "flag_stub: 32-bit fields within ldr w immediates");
const bool g_regbase = [] { const char *v = std::getenv("PS2_VU_JIT_REGBASE"); return !v || std::strcmp(v, "0") != 0; }();
const bool g_flagstub = [] { const char *v = std::getenv("PS2_VU_JIT_FLAGSTUB"); return !v || std::strcmp(v, "0") != 0; }();
const bool g_pin_const = [] { const char *v = std::getenv("PS2_VU_JIT_PINCONST"); return !v || std::strcmp(v, "0") != 0; }();
const bool g_movi = [] { const char *v = std::getenv("PS2_VU_JIT_MOVI"); return !v || std::strcmp(v, "0") != 0; }();
const bool g_retarget = [] { const char *v = std::getenv("PS2_VU_JIT_RETARGET"); return !v || std::strcmp(v, "0") != 0; }();  // regs(1) and g_vu1_mem relative to g_base (init_base)

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
constexpr unsigned SP = 31, ZR = 31, X16 = 16, X17 = 17, BASE = 19;
// x29 = the VU1 registers (regs(1) = x19 + kParamOffset[0]): the flag/register fields sit ~48 MB above
// x19, so without it every access to them took mov/movk/add before the load (PS2_VU_JIT_REGBASE=0: off).
constexpr unsigned REGS = 29;
constexpr unsigned kIntRegs[] = {9, 10, 11, 12, 13, 14, 15, 2};
constexpr unsigned kMaxDepth = 8;  // the translator's expressions need <= 7
// Register locals (Compiler::allocate_locals). v0..v2 are scratch, v16..v23 the operand stack.
// x20..x28 are callee-saved; x3..x8 and every vector register are saved around import calls.
constexpr unsigned kIntLocalRegs[] = {20, 21, 22, 23, 24, 25, 26, 27, 28, 3, 4, 5, 6, 7, 8};
constexpr unsigned kVecLocalRegs[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 26, 27, 28, 29, 30, 31};
bool caller_saved_int(unsigned r) { return r >= 3 && r <= 8; }

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
        // flag routines called by bl (mac / status / clip)
        std::size_t stub_at[3] = {0, 0, 0};
        for (const auto &[at, kind] : stub_calls_) {
            if (!stub_at[kind]) {
                stub_at[kind] = code_.size();
                flag_stub(kind);
            }
            const std::int64_t delta = static_cast<std::int64_t>(stub_at[kind]) - static_cast<std::int64_t>(at);
            if (!fits(delta, 26)) range_error_ = true;
            code_[at] |= static_cast<std::uint32_t>(delta) & 0x3FFFFFFu;
        }
        // literal pool
        while (code_.size() % 4) code_.push_back(0xD4200000u);  // brk (never reached)
        std::map<std::array<std::uint32_t, 4>, std::size_t> pool;
        for (const auto &[at, bytes] : pool_refs_) {
            auto found = pool.find(bytes);
            if (found == pool.end()) {
                found = pool.emplace(bytes, code_.size()).first;
                for (int k = 0; k < 4; ++k) code_.push_back(bytes[k]);
            }
            const std::size_t word = found->second;
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
            allocate_locals();
            return true;
        }
        return false;
    }

    // Register locals: the most used integer locals live in x20..x28 (callee-saved, survive the
    // import calls), the most used vector locals in v3..v15 (saved to their frame slots around calls;
    // d8..d15 are saved in the prologue as the ABI requires). The rest stay in the frame.
    void allocate_locals() {
        std::vector<std::uint32_t> uses(local_type_.size(), 0), writes(local_type_.size(), 0);
        std::map<std::uint32_t, std::uint32_t> const_uses;
        Reader r = rd_;
        unsigned depth = 0;
        bool done = false;
        while (!done && r.ok && r.p < r.end) {
            const std::uint8_t op = r.byte();
            switch (op) {
            case 0x02: case 0x03: case 0x04: r.byte(); ++depth; break;
            case 0x0B:
                if (depth == 0) done = true;
                else --depth;
                break;
            case 0x20: case 0x21: case 0x22: {
                const std::uint32_t l = r.u32();
                if (l < uses.size()) {
                    ++uses[l];
                    if (op != 0x20) ++writes[l];
                }
                break;
            }
            case 0xFD: {  // count splat v128.const values: candidates for a pinned register
                Reader peek = r;
                if (peek.u32() == 0x0C) {
                    std::uint32_t lane[4] = {};
                    for (int k = 0; k < 16; ++k) lane[k / 4] |= static_cast<std::uint32_t>(peek.byte()) << (8 * (k % 4));
                    if (lane[0] == lane[1] && lane[0] == lane[2] && lane[0] == lane[3]) ++const_uses[lane[0]];
                }
                skip_immediates_in(r, op);
                break;
            }
            default: skip_immediates_in(r, op); break;
            }
        }
        // The params are always the VU1 registers and data memory (vu_micro.cpp calls programs with
        // regs(1) / g_vu1_mem; the prologue checks it): never written, they read as constants.
        for (unsigned p = 0; p < 2 && p < uses.size(); ++p) const_param_[p] = !writes[p];
        std::vector<std::uint32_t> order(local_type_.size());
        for (std::uint32_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) { return uses[a] > uses[b]; });
        local_reg_.assign(local_type_.size(), -1);
        unsigned next_int = 0, next_vec = 0;
        // Splat constants used often (the clamp masks: thousands of times in the big programs) get a vector
        // register for the whole program when they are used more than the vector local they displace: each
        // use is then no instruction at all (PS2_VU_JIT_PINCONST=0: off). Ranked against the vector locals.
        if (g_pin_const) {
            std::vector<std::uint32_t> vec_counts;
            for (std::uint32_t l = 0; l < local_type_.size(); ++l)
                if (uses[l] && !(local_type_[l] == I32 || local_type_[l] == I64)) vec_counts.push_back(uses[l]);
            std::sort(vec_counts.rbegin(), vec_counts.rend());
            std::vector<std::pair<std::uint32_t, std::uint32_t>> cands;  // (uses, value)
            for (const auto &[value, n] : const_uses)
                if (n >= 16) cands.emplace_back(n, value);
            std::sort(cands.rbegin(), cands.rend());
            const unsigned regs = sizeof kVecLocalRegs / sizeof kVecLocalRegs[0];
            unsigned pinned = 0;
            for (const auto &[n, value] : cands) {
                if (pinned >= kMaxPinned) break;
                // the local that would lose its register: the (regs - pinned)-th most used one
                const std::size_t loser = regs - pinned - 1;
                if (loser < vec_counts.size() && vec_counts[loser] >= n) break;
                pinned_.push_back({value, kVecLocalRegs[regs - 1 - pinned]});
                ++pinned;
            }
            vec_regs_avail_ = regs - pinned;
        }
        for (std::uint32_t l : order) {
            if (!uses[l]) break;
            if (local_type_[l] == I32 || local_type_[l] == I64) ++locals_used_int_;
            else ++locals_used_vec_;
            if (local_type_[l] == I32 || local_type_[l] == I64) {
                if (l < 2 && !writes[l]) continue;  // the (r, mem) params: constants, see const_param_
                if (next_int < sizeof kIntLocalRegs / sizeof kIntLocalRegs[0]) {
                    local_reg_[l] = static_cast<int>(kIntLocalRegs[next_int++]);
                    if (caller_saved_int(static_cast<unsigned>(local_reg_[l]))) scratch_int_locals_.push_back(l);
                }
            } else if (next_vec < vec_regs_avail_) {
                local_reg_[l] = static_cast<int>(kVecLocalRegs[next_vec++]);
                vec_locals_.push_back(l);
            }
        }
    }
    bool is_vec(Type t) const { return t == F64 || t == V128; }
    // Vector register locals are caller-saved: store them before an import call, reload after.
    void save_vec_locals() {
        for (std::uint32_t l : vec_locals_) frame_st(V128, static_cast<unsigned>(local_reg_[l]), local_off_[l]);
        for (std::uint32_t l : scratch_int_locals_) frame_st(I64, static_cast<unsigned>(local_reg_[l]), local_off_[l]);
    }
    // The pinned constants (caller-saved vector registers): set in the prologue and again after a call.
    void load_pinned() {
        for (const auto &[value, reg] : pinned_)
            if (!splat_imm(reg, value)) {
                pool_refs_.push_back({code_.size(), {value, value, value, value}});
                w(0x9C000000u | reg);  // ldr q, literal
            }
    }
    void load_vec_locals() {
        load_pinned();
        for (std::uint32_t l : vec_locals_) frame_ld(V128, static_cast<unsigned>(local_reg_[l]), local_off_[l]);
        for (std::uint32_t l : scratch_int_locals_) frame_ld(I64, static_cast<unsigned>(local_reg_[l]), local_off_[l]);
    }

    // Instructions that write all of Rd (bits 0-4) without reading it: an op followed by local.set can
    // write the local's register directly instead of its stack slot + a mov (PS2_VU_JIT_RETARGET=0: off).
    static bool writes_rd_only(std::uint32_t i, bool vec) {
        if (vec) {
            if ((i & 0x9F200400u) == 0x0E200400u) {  // AdvSIMD three same
                const std::uint32_t opc = (i >> 11) & 0x1Fu, u = (i >> 29) & 1u, size = (i >> 22) & 3u;
                if (opc == 0x03 && u && size) return false;              // bsl / bit / bif
                if (opc == 0x19 || opc == 0x12 || opc == 0x0F) return false;  // fmla/fmls, mla/mls, saba
                return true;
            }
            if ((i & 0x9F3E0C00u) == 0x0E200800u) {  // AdvSIMD two-reg misc: whole-register results only
                switch ((i >> 12) & 0x1Fu) {
                case 0x05: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E:
                case 0x0F: case 0x18: case 0x19: case 0x1B: case 0x1C: case 0x1D: case 0x1F: return true;
                default: return false;
                }
            }
            if ((i & 0xBFE0FC00u) == 0x0E000400u) return true;  // dup (element)
            if ((i & 0xBFE0FC00u) == 0x0E000C00u) return true;  // dup (general)
            if ((i & 0xBFE09C00u) == 0x0E000000u) return true;  // tbl
            if ((i & 0xFF000000u) == 0x9C000000u) return true;  // ldr q, literal
            if ((i & 0xFFC00000u) == 0x3DC00000u) return true;  // ldr q, [x, #imm]
            if ((i & 0x9FF80400u) == 0x0F000400u) {  // movi / mvni / fmov (vector immediate), not orr / bic
                const std::uint32_t cmode = (i >> 12) & 0xFu;
                return !(cmode & 1u) || cmode == 13 || cmode == 15;
            }
            return false;
        }
        if ((i & 0x1F800000u) == 0x11000000u) return true;                                  // add/sub imm
        if ((i & 0x1F000000u) == 0x0B000000u) return true;                                  // add/sub reg
        if ((i & 0x1F000000u) == 0x0A000000u) return true;                                  // logical reg
        if ((i & 0x1F800000u) == 0x12000000u) return true;                                  // logical imm
        if ((i & 0x1F800000u) == 0x12800000u) return (i & 0x60000000u) != 0x60000000u;     // movn/movz, not movk
        if ((i & 0x1F800000u) == 0x13000000u) return (i & 0x60000000u) != 0x20000000u;     // sbfm/ubfm, not bfm
        if ((i & 0x5FE00000u) == 0x1AC00000u) return true;                                  // lslv/lsrv/asrv/rorv/div
        if ((i & 0x1FE00000u) == 0x1A800000u) return true;                                  // csel family
        if ((i & 0x1F000000u) == 0x1B000000u) return true;                                  // madd/msub/umulh
        if ((i & 0x3F400000u) == 0x39400000u) return true;                                  // ldr*, unsigned offset
        if ((i & 0xBFE0FC00u) == 0x0E003C00u) return true;                                  // umov
        return false;
    }
    // Let the last instruction write `to` instead of `from` (its Rd), when nothing branches in between.
    bool retarget_last(unsigned from, unsigned to, bool vec) {
        if (!g_retarget || code_.size() <= barrier_ || code_.empty()) return false;
        std::uint32_t &i = code_.back();
        if ((i & 31u) != from || !writes_rd_only(i, vec)) return false;
        i = (i & ~31u) | to;
        ++retargeted_;
        return true;
    }

    // A 4 x 32-bit splat constant in one instruction (movi / mvni / fmov) when it has such a form: the
    // clamp masks 0x7fffffff, 0x80000000, 0x00800000 and 0 were 80 % of the literal-pool loads.
    bool splat_imm(unsigned d, std::uint32_t v) {
        const auto modimm = [&](std::uint32_t op, std::uint32_t cmode, std::uint32_t imm8) {
            w(0x4F000400u | op << 29 | (imm8 >> 5) << 16 | cmode << 12 | (imm8 & 31u) << 5 | d);
        };
        if (v == 0) {
            w(0x6F00E400u | d);  // movi v.2d, #0
            return true;
        }
        for (std::uint32_t sh = 0; sh < 4; ++sh) {
            const std::uint32_t mask = 0xFFu << (8 * sh);
            if (!(v & ~mask)) { modimm(0, 2 * sh, v >> (8 * sh)); return true; }     // movi .4s, #imm, lsl
            if (!(~v & ~mask)) { modimm(1, 2 * sh, ~v >> (8 * sh)); return true; }   // mvni .4s, #imm, lsl
        }
        if ((v & 0xFFFF00FFu) == 0xFFu) { modimm(0, 12, (v >> 8) & 0xFFu); return true; }        // movi msl #8
        if ((v & 0xFF00FFFFu) == 0xFFFFu) { modimm(0, 13, (v >> 16) & 0xFFu); return true; }     // movi msl #16
        if ((~v & 0xFFFF00FFu) == 0xFFu) { modimm(1, 12, (~v >> 8) & 0xFFu); return true; }      // mvni msl #8
        if ((~v & 0xFF00FFFFu) == 0xFFFFu) { modimm(1, 13, (~v >> 16) & 0xFFu); return true; }   // mvni msl #16
        // fmov .4s, #imm: a:NOT(b):bbbbb:cdefgh:0{19}
        if (!(v & 0x7FFFFu)) {
            const std::uint32_t b = (v >> 29) & 1u, b5 = (v >> 25) & 0x1Fu;
            if (((v >> 30) & 1u) == (b ^ 1u) && b5 == (b ? 0x1Fu : 0u)) {
                modimm(0, 15, (v >> 31) << 7 | b << 6 | ((v >> 19) & 0x3Fu));
                return true;
            }
        }
        return false;
    }

    // FlagPipe::mac() / status() / clip() (vu_ops.hpp) for x0 = &FlagPipe, result in w0: the newest of the
    // 8 history entries at least 4 cycles old, else the oldest. Uses x1, x16, x17 only.
    void flag_stub(std::uint32_t kind) {
        using vuops::FlagPipe;
        const bool clip = kind == 2;
        const std::uint32_t ts = clip ? offsetof(FlagPipe, clip_ts) : offsetof(FlagPipe, mac_ts);
        const std::uint32_t val = kind == 0 ? offsetof(FlagPipe, mac_v) : kind == 1 ? offsetof(FlagPipe, status_v) : offsetof(FlagPipe, clip_v);
        const auto ldr_w = [&](unsigned t, unsigned n, std::uint32_t off) { w(0xB9400000u | (off / 4) << 10 | n << 5 | t); };
        ldr_w(17, 0, offsetof(FlagPipe, t));
        if (clip) {
            ldr_w(16, 0, offsetof(FlagPipe, stalls));
            w(0x0B100231u);  // add w17, w17, w16 (now() = t + stalls)
        }
        ldr_w(1, 0, clip ? offsetof(FlagPipe, ch) : offsetof(FlagPipe, mh));
        std::size_t hits[8];
        for (std::size_t &h : hits) {
            w(0x51000421u);  // sub w1, w1, #1
            w(0x12000830u);  // and w16, w1, #7
            w(0x8B304810u);  // add x16, x0, w16, uxtw #2
            ldr_w(16, 16, ts);
            w(0x11001210u);  // add w16, w16, #4
            w(0x6B11021Fu);  // cmp w16, w17
            h = code_.size();
            w(0x54000000u | LS);  // b.ls found
        }
        const std::size_t found = code_.size();  // falls through with w1 = head - 8
        for (std::size_t h : hits) code_[h] |= static_cast<std::uint32_t>(found - h) << 5;
        w(0x12000830u);  // and w16, w1, #7
        w(0x8B304810u);  // add x16, x0, w16, uxtw #2
        ldr_w(0, 16, val);
        w(0xD65F03C0u);  // ret
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
        if (!g_regbase) w(0x910003FDu);  // mov x29, sp (no frame record with the register base)
        w(0xA9BF53F3u);  // stp x19, x20, [sp, #-16]!
        w(0xA9BF5BF5u);  // stp x21, x22, [sp, #-16]!
        w(0xA9BF63F7u);  // stp x23, x24, [sp, #-16]!
        w(0xA9BF6BF9u);  // stp x25, x26, [sp, #-16]!
        w(0xA9BF73FBu);  // stp x27, x28, [sp, #-16]!
        w(0x6DBF27E8u);  // stp d8, d9, [sp, #-16]!
        w(0x6DBF2FEAu);  // stp d10, d11, [sp, #-16]!
        w(0x6DBF37ECu);  // stp d12, d13, [sp, #-16]!
        w(0x6DBF3FEEu);  // stp d14, d15, [sp, #-16]!
        sub_sp(frame_);
        movx(BASE, g_base);
        if (g_regbase) movx(REGS, g_base + kParamOffset[0]);
        w(0x6F00E400u);  // movi v0.2d, #0
        for (std::uint32_t off = 0; off < locals_size_; off += 16) frame_st(V128, 0, off);
        for (std::uint32_t l = 0; l < local_reg_.size(); ++l) {
            if (local_reg_[l] < 0) continue;
            const unsigned reg = static_cast<unsigned>(local_reg_[l]);
            if (is_vec(local_type_[l])) vmov(reg, 0);
            else w(0xAA1F03E0u | reg);  // mov x, xzr
        }
        // params as offsets from the base
        w(0xCB000000u | BASE << 16 | 0 << 5 | 0);  // sub x0, x0, x19
        w(0xCB000000u | BASE << 16 | 1 << 5 | 1);  // sub x1, x1, x19
        for (unsigned p = 0; p < 2; ++p) {
            if (const_param_[p]) {  // must be the expected object: else trap (brk) instead of wrong data
                movx(X17, kParamOffset[p]);
                w(0xEB00001Fu | X17 << 16 | p << 5);  // cmp xp, x17
                w(0x54000040u | NE);                  // b.ne +2
                w(0x14000002u);                       // b +2
                w(0xD4200000u);                       // brk #0
                continue;
            }
            if (local_reg_[p] >= 0) w(0x2A0003E0u | p << 16 | static_cast<unsigned>(local_reg_[p]));  // mov w, wp
            else frame_st(I32, p, local_off_[p]);
        }
        load_pinned();
    }
    void epilogue() {
        add_sp(frame_);
        w(0x6CC13FEEu);  // ldp d14, d15, [sp], #16
        w(0x6CC137ECu);  // ldp d12, d13, [sp], #16
        w(0x6CC12FEAu);  // ldp d10, d11, [sp], #16
        w(0x6CC127E8u);  // ldp d8, d9, [sp], #16
        w(0xA8C173FBu);  // ldp x27, x28, [sp], #16
        w(0xA8C16BF9u);  // ldp x25, x26, [sp], #16
        w(0xA8C163F7u);  // ldp x23, x24, [sp], #16
        w(0xA8C15BF5u);  // ldp x21, x22, [sp], #16
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
        max_depth_ = std::max(max_depth_, stack_.size());
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
        barrier_ = code_.size();
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
        // mac / status / clip: a native routine at the program's end (flag_stub) that touches only x0, x1,
        // x16, x17, x30 and NZCV, so neither the locals nor the operand stack need saving around it.
        if (!tail && g_flagstub && f <= 2) {
            const unsigned a = ireg(top());
            w(0x8B204000u | a << 16 | BASE << 5 | 0);  // add x0, x19, wA, uxtw (the FlagPipe)
            stub_calls_.push_back({code_.size(), f});
            w(0x94000000u);                           // bl stub (patched in compile())
            stack_.pop_back();
            if (!push(I32)) return false;
            w(0x2A0003E0u | 0 << 16 | ireg(top()));  // mov w, w0
            return true;
        }
        const std::size_t base = stack_.size() - imp.params;
        for (std::size_t d = 0; d < base; ++d) frame_st(stack_[d], reg_of(d), spill_off_ + 16u * static_cast<std::uint32_t>(d));
        for (std::size_t k = 0; k < imp.params; ++k) frame_st(I64, ireg(base + k), args_off_ + 8u * static_cast<std::uint32_t>(k));
        // Save the register locals BEFORE the arguments go to x0..x3: integer locals may live in x3..x8,
        // and a 4-argument import (efu) loads x3 - saving afterwards stored the argument as the local's
        // value, so the local came back wrong (broken lighting / reflections: EFU normalises vectors).
        save_vec_locals();
        for (std::size_t k = 0; k < imp.params; ++k) frame_ld(I32, static_cast<unsigned>(k), args_off_ + 8u * static_cast<std::uint32_t>(k));
        movx(X16, reinterpret_cast<std::uint64_t>(imp.fn));
        w(0xD63F0000u | X16 << 5);  // blr x16
        stack_.resize(base);
        if (tail) {
            epilogue();
            return true;
        }
        load_vec_locals();
        for (std::size_t d = 0; d < base; ++d) frame_ld(stack_[d], reg_of(d), spill_off_ + 16u * static_cast<std::uint32_t>(d));
        if (imp.result) {
            if (!push(I32)) return false;
            w(0x2A0003E0u | 0 << 16 | ireg(top()));  // mov w, w0
        }
        return true;
    }

    // Skips one instruction's immediates (dead code after br / br_table / return).
    bool skip_immediates(std::uint8_t op) { return skip_immediates_in(rd_, op); }
    static bool skip_immediates_in(Reader &r, std::uint8_t op) {
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


    // ---------- lazy operand stack
    // An entry can stay un-materialised: a register local (Reg), a constant (Imm) or the result of a
    // compare still in NZCV (Cond). Instructions with a fast path read such entries directly; all
    // others get their operands materialised into the slot registers and use instruction_slots().
    enum LazyKind : std::uint8_t { kSlot, kReg, kImm, kCond };
    struct Lazy {
        LazyKind kind = kSlot;
        unsigned reg = 0;        // kReg: the local's register; kCond: the condition code (true when set)
        std::uint64_t imm = 0;   // kImm
    };
    std::vector<Lazy> lz_;

    void sync_lazy() { lz_.resize(stack_.size()); }
    bool is_int(std::size_t d) const { return stack_[d] == I32 || stack_[d] == I64; }
    void materialize(std::size_t d) {
        Lazy &e = lz_[d];
        switch (e.kind) {
        case kSlot: return;
        case kReg:
            if (is_int(d)) w(0xAA0003E0u | e.reg << 16 | ireg(d));  // mov x, xl
            else vmov(vreg(d), e.reg);
            break;
        case kImm:
            if (stack_[d] == I64) movx(ireg(d), e.imm);
            else movw(ireg(d), static_cast<std::uint32_t>(e.imm));
            break;
        case kCond: w(0x1A9F07E0u | (e.reg ^ 1u) << 12 | ireg(d)); break;  // cset
        }
        e = Lazy{};
    }
    void materialize_all() {
        for (std::size_t d = 0; d < stack_.size(); ++d) materialize(d);
    }
    void materialize_cond() {
        for (std::size_t d = 0; d < stack_.size(); ++d)
            if (lz_[d].kind == kCond) materialize(d);
    }
    void materialize_reg(unsigned reg, std::size_t below) {
        for (std::size_t d = 0; d < below && d < stack_.size(); ++d)
            if (lz_[d].kind == kReg && lz_[d].reg == reg) materialize(d);
    }
    // Source register of an integer entry (constants go to `scratch`, conditions to their slot).
    unsigned isrc(std::size_t d, unsigned scratch = X17) {
        const Lazy &e = lz_[d];
        if (e.kind == kReg) return e.reg;
        if (e.kind == kImm) {
            if (e.imm == 0) return ZR;
            if (stack_[d] == I64) movx(scratch, e.imm);
            else movw(scratch, static_cast<std::uint32_t>(e.imm));
            return scratch;
        }
        if (e.kind == kCond) materialize(d);
        return ireg(d);
    }
    // As isrc, but never xzr: for the immediate forms (cmp #imm, add #imm), where register 31 is sp.
    unsigned isrc_nz(std::size_t d, unsigned scratch) {
        if (lz_[d].kind == kImm && lz_[d].imm == 0) {
            w(0xAA1F03E0u | scratch);  // mov x, xzr
            return scratch;
        }
        return isrc(d, scratch);
    }
    unsigned vsrc(std::size_t d) const { return lz_[d].kind == kReg ? lz_[d].reg : vreg(d); }
    void set_slot(std::size_t d) { lz_[d] = Lazy{}; }

    static bool logical_imm(std::uint64_t imm, bool wide, std::uint32_t &out) {
        if (!wide) imm = (imm & 0xFFFFFFFFull) | (imm << 32);
        if (imm == 0 || imm == ~0ull) return false;
        unsigned size = 64;
        while (size > 2) {
            size /= 2;
            const std::uint64_t mask = (1ull << size) - 1;
            if ((imm & mask) != ((imm >> size) & mask)) {
                size *= 2;
                break;
            }
        }
        const std::uint64_t mask = size == 64 ? ~0ull : (1ull << size) - 1;
        imm &= mask;
        const auto is_mask = [](std::uint64_t v) { return v && ((v + 1) & v) == 0; };
        const auto is_shifted_mask = [&](std::uint64_t v) { return v && is_mask((v - 1) | v); };
        unsigned i, cto;
        if (is_shifted_mask(imm)) {
            i = static_cast<unsigned>(__builtin_ctzll(imm));
            cto = static_cast<unsigned>(__builtin_ctzll(~(imm >> i)));
        } else {
            imm |= ~mask;
            if (!is_shifted_mask(~imm)) return false;
            const unsigned clo = static_cast<unsigned>(__builtin_clzll(~imm));
            i = 64 - clo;
            cto = clo + static_cast<unsigned>(__builtin_ctzll(~imm)) - (64 - size);
        }
        const unsigned immr = (size - i) & (size - 1);
        std::uint64_t nimms = ~static_cast<std::uint64_t>(size - 1) << 1;
        nimms |= (cto - 1);
        const unsigned n = static_cast<unsigned>(((nimms >> 6) & 1) ^ 1);
        out = n << 12 | immr << 6 | static_cast<unsigned>(nimms & 0x3F);
        return true;
    }

    // Constant folding for the integer ops the translator emits.
    static bool fold(std::uint8_t op, bool wide, std::uint64_t a, std::uint64_t b, std::uint64_t &out) {
        if (!wide) {
            const auto x = static_cast<std::uint32_t>(a), y = static_cast<std::uint32_t>(b);
            std::uint32_t r;
            switch (op) {
            case 0x6A: r = x + y; break;
            case 0x6B: r = x - y; break;
            case 0x6C: r = x * y; break;
            case 0x71: r = x & y; break;
            case 0x72: r = x | y; break;
            case 0x73: r = x ^ y; break;
            case 0x74: r = x << (y & 31); break;
            case 0x75: r = static_cast<std::uint32_t>(static_cast<std::int32_t>(x) >> (y & 31)); break;
            case 0x76: r = x >> (y & 31); break;
            default: return false;
            }
            out = r;
            return true;
        }
        switch (op) {
        case 0x7C: out = a + b; return true;
        case 0x7D: out = a - b; return true;
        case 0x83: out = a & b; return true;
        case 0x84: out = a | b; return true;
        case 0x85: out = a ^ b; return true;
        case 0x86: out = a << (b & 63); return true;
        case 0x88: out = a >> (b & 63); return true;
        default: return false;
        }
    }

    // i32 / i64 binary op with lazy operands. Returns false when the op isn't one of them.
    bool lazy_binary(std::uint8_t op) {
        std::uint32_t reg_enc = 0;
        bool wide_op = false, commutative = false;
        switch (op) {
        case 0x6A: reg_enc = 0x0B000000u; commutative = true; break;
        case 0x6B: reg_enc = 0x4B000000u; break;
        case 0x6C: reg_enc = 0x1B007C00u; commutative = true; break;
        case 0x71: reg_enc = 0x0A000000u; commutative = true; break;
        case 0x72: reg_enc = 0x2A000000u; commutative = true; break;
        case 0x73: reg_enc = 0x4A000000u; commutative = true; break;
        case 0x74: reg_enc = 0x1AC02000u; break;
        case 0x75: reg_enc = 0x1AC02800u; break;
        case 0x76: reg_enc = 0x1AC02400u; break;
        case 0x7C: reg_enc = 0x8B000000u; wide_op = true; commutative = true; break;
        case 0x7D: reg_enc = 0xCB000000u; wide_op = true; break;
        case 0x83: reg_enc = 0x8A000000u; wide_op = true; commutative = true; break;
        case 0x84: reg_enc = 0xAA000000u; wide_op = true; commutative = true; break;
        case 0x85: reg_enc = 0xCA000000u; wide_op = true; commutative = true; break;
        case 0x86: reg_enc = 0x9AC02000u; wide_op = true; break;
        case 0x88: reg_enc = 0x9AC02400u; wide_op = true; break;
        default: return false;
        }
        if (!need(2)) return true;
        std::size_t a = top() - 1, b = top();
        const bool wide = stack_[a] == I64;
        if (wide != wide_op) {
            fail("binary op type");
            return true;
        }
        Lazy la = lz_[a], lb = lz_[b];
        if (la.kind == kImm && lb.kind == kImm) {
            std::uint64_t v;
            if (fold(op, wide, la.imm, lb.imm, v)) {
                stack_.pop_back();
                sync_lazy();
                lz_[a] = Lazy{kImm, 0, v};
                return true;
            }
        }
        // constant on the left of a commutative op: use it as the right operand
        bool swapped = false;
        if (la.kind == kImm && lb.kind != kImm && commutative) {
            std::swap(la, lb);
            swapped = true;
        }
        const unsigned d = ireg(a);
        const unsigned sf = wide ? 0x80000000u : 0u;
        if (lb.kind == kImm) {
            // the register operand: the non-constant one
            const std::size_t src_d = swapped ? b : a;
            const unsigned n = isrc_nz(src_d, X16);
            const std::uint64_t imm = wide ? lb.imm : (lb.imm & 0xFFFFFFFFull);
            bool done = false;
            switch (op) {
            case 0x6A: case 0x7C: case 0x6B: case 0x7D: {
                const bool sub = op == 0x6B || op == 0x7D;
                std::int64_t v = wide ? static_cast<std::int64_t>(imm) : static_cast<std::int32_t>(static_cast<std::uint32_t>(imm));
                bool is_sub = sub;
                if (v < 0) {
                    v = -v;
                    is_sub = !is_sub;
                }
                if (v >= 0 && v < 4096) {
                    w((is_sub ? 0x51000000u : 0x11000000u) | sf | static_cast<std::uint32_t>(v) << 10 | n << 5 | d);
                    done = true;
                } else if (v >= 0 && (v & 0xFFF) == 0 && v < (1 << 24)) {
                    w((is_sub ? 0x51400000u : 0x11400000u) | sf | static_cast<std::uint32_t>(v >> 12) << 10 | n << 5 | d);
                    done = true;
                }
                break;
            }
            case 0x71: case 0x72: case 0x73: case 0x83: case 0x84: case 0x85: {
                std::uint32_t bits;
                if (logical_imm(imm, wide, bits)) {
                    const std::uint32_t base = (op == 0x71 || op == 0x83) ? 0x12000000u : (op == 0x72 || op == 0x84) ? 0x32000000u : 0x52000000u;
                    w(base | sf | bits << 10 | n << 5 | d);
                    done = true;
                }
                break;
            }
            case 0x74: case 0x86: {
                const unsigned size = wide ? 64 : 32, sh = static_cast<unsigned>(imm & (size - 1));
                if (!sh) w((wide ? 0xAA0003E0u : 0x2A0003E0u) | n << 16 | d);
                else w((wide ? 0xD3400000u : 0x53000000u) | ((size - sh) & (size - 1)) << 16 | (size - 1 - sh) << 10 | n << 5 | d);
                done = true;
                break;
            }
            case 0x76: case 0x88: {
                const unsigned size = wide ? 64 : 32, sh = static_cast<unsigned>(imm & (size - 1));
                if (!sh) w((wide ? 0xAA0003E0u : 0x2A0003E0u) | n << 16 | d);
                else w((wide ? 0xD340FC00u : 0x53007C00u) | sh << 16 | n << 5 | d);
                done = true;
                break;
            }
            case 0x75: {
                const unsigned sh = static_cast<unsigned>(imm & 31);
                if (!sh) w(0x2A0003E0u | n << 16 | d);
                else w(0x13007C00u | sh << 16 | n << 5 | d);
                done = true;
                break;
            }
            default: break;
            }
            if (!done) {
                if (wide) movx(X17, imm);
                else movw(X17, static_cast<std::uint32_t>(imm));
                w(reg_enc | X17 << 16 | n << 5 | d);
            }
        } else {
            const unsigned n = isrc(a, X16), m = isrc(b, X17);
            w(reg_enc | m << 16 | n << 5 | d);
        }
        stack_.pop_back();
        sync_lazy();
        set_slot(a);
        return true;
    }

    // Integer compares: leave the result in NZCV (kCond).
    bool lazy_compare(std::uint8_t op) {
        Cond c;
        switch (op) {
        case 0x46: c = EQ; break;
        case 0x47: c = NE; break;
        case 0x48: c = LT; break;
        case 0x49: c = LO; break;
        case 0x4A: c = GT; break;
        case 0x4B: c = HI; break;
        case 0x4C: c = LE; break;
        case 0x4D: c = LS; break;
        case 0x4E: c = GE; break;
        case 0x4F: c = HS; break;
        default: return false;
        }
        if (!need(2)) return true;
        const std::size_t a = top() - 1, b = top();
        const bool wide = stack_[a] == I64;
        const unsigned n = isrc_nz(a, X16);
        const Lazy lb = lz_[b];
        const std::uint64_t imm = wide ? lb.imm : (lb.imm & 0xFFFFFFFFull);
        if (lb.kind == kImm && imm < 4096) {
            w((wide ? 0xF100001Fu : 0x7100001Fu) | static_cast<std::uint32_t>(imm) << 10 | n << 5);  // cmp n, #imm
        } else {
            const unsigned m = isrc(b, X17);
            w((wide ? 0xEB00001Fu : 0x6B00001Fu) | m << 16 | n << 5);  // cmp n, m
        }
        stack_.pop_back();
        sync_lazy();
        stack_[a] = I32;
        lz_[a] = Lazy{kCond, static_cast<unsigned>(c), 0};
        return true;
    }

    // A constant address (offset from x19): x19 + disp when the scaled immediate fits, else x29 (the
    // VU1 registers) + disp - kParamOffset[0] when that does.
    void const_base(std::uint64_t disp, std::uint32_t scale, unsigned &basereg, std::uint64_t &d) const {
        basereg = BASE;
        d = disp;
        if (!g_regbase || (disp % scale == 0 && disp / scale < 4096) || disp < kParamOffset[0]) return;
        const std::uint64_t rel = disp - kParamOffset[0];
        if ((rel % scale == 0 && rel / scale < 4096) || rel < 4096) {
            basereg = REGS;
            d = rel;
        }
    }

    // Integer memory access with a lazy address / value.
    bool lazy_mem(bool store, unsigned size_log2, bool sign, std::uint32_t off) {
        if (!need(store ? 2 : 1)) return true;
        const std::size_t ad = store ? top() - 1 : top();
        const std::uint32_t scale = 1u << size_log2;
        unsigned basereg;
        std::uint64_t disp;
        if (lz_[ad].kind == kImm) {
            const_base((lz_[ad].imm & 0xFFFFFFFFull) + off, scale, basereg, disp);
        } else {
            address(isrc(ad, X16));
            basereg = X16;
            disp = off;
        }
        if (disp % scale || disp / scale >= 4096) {
            add_imm_x(X16, basereg, static_cast<std::uint32_t>(disp));
            basereg = X16;
            disp = 0;
        }
        const auto field = static_cast<std::uint32_t>(disp / scale) << 10 | basereg << 5;
        if (store) {
            static const std::uint32_t kSt[4] = {0x39000000u, 0x79000000u, 0xB9000000u, 0xF9000000u};
            const unsigned v = isrc(top(), X17);
            w(kSt[size_log2] | field | v);
            stack_.pop_back();
            stack_.pop_back();
            sync_lazy();
            return true;
        }
        static const std::uint32_t kLd[4] = {0x39400000u, 0x79400000u, 0xB9400000u, 0xF9400000u};
        static const std::uint32_t kLdS[2] = {0x39C00000u, 0x79C00000u};
        w((sign ? kLdS[size_log2] : kLd[size_log2]) | field | ireg(ad));
        stack_.back() = size_log2 == 3 ? I64 : I32;
        set_slot(ad);
        return true;
    }

    bool lazy_vmem(bool store, std::uint32_t off) {
        if (!need(store ? 2 : 1)) return true;
        const std::size_t ad = store ? top() - 1 : top();
        unsigned basereg;
        std::uint64_t disp;
        if (lz_[ad].kind == kImm) {
            const_base((lz_[ad].imm & 0xFFFFFFFFull) + off, 16, basereg, disp);
        } else {
            address(isrc(ad, X16));
            basereg = X16;
            disp = off;
        }
        if (disp % 16 || disp / 16 >= 4096) {
            add_imm_x(X16, basereg, static_cast<std::uint32_t>(disp));
            basereg = X16;
            disp = 0;
        }
        const auto field = static_cast<std::uint32_t>(disp / 16) << 10 | basereg << 5;
        if (store) {
            w(0x3D800000u | field | vsrc(top()));
            stack_.pop_back();
            stack_.pop_back();
            sync_lazy();
            return true;
        }
        stack_.back() = V128;
        set_slot(ad);
        // Store-to-load forwarding: "str qM, [b, #o]" directly followed by "ldr qD, [b, #o]" (VU register
        // written, then read by the next op) waited for the store's round trip - the hottest stall of the
        // car-select programs. The value is still in vM: copy it (the store stays). Not across a jump
        // target (barrier_). PS2_VU_JIT_FWD=0: off.
        static const bool fwd = [] { const char *v = std::getenv("PS2_VU_JIT_FWD"); return !v || v[0] != '0'; }();
        if (fwd) {
            const int m = forwardable(field, basereg, static_cast<std::uint32_t>(disp));
            if (m >= 0) {
                const unsigned d = vreg(ad), mm = static_cast<unsigned>(m);
                w(0x4EA01C00u | mm << 16 | mm << 5 | d);  // orr vd.16b, vm.16b, vm.16b (mov; harmless if d == m)
                ++fwd_hits_;
                return true;
            }
        }
        w(0x3DC00000u | field | vreg(ad));
        return true;
    }

    // Windowed store-to-load forwarding (PS2_VU_JIT_FWD): the newest "str qM, [base, #disp]" within the last
    // kFwdWindow instructions of this straight-line stretch, if vM still holds the stored value and nothing
    // else may have written those 16 bytes. Typical case: a lower op merges into a VU register and stores
    // it, the upper op stores another one, and both are read back a few instructions later to rebuild
    // their clamped copies - each reload waited for its store. Deliberately conservative: only fixed base
    // registers (not x16), any call / any write to vM / any store not provably elsewhere stops the search.
    // Returns M or -1.
    int forwardable(std::uint32_t field, unsigned basereg, std::uint32_t disp) const {
        constexpr std::size_t kFwdWindow = 48;
        if (basereg == X16 || basereg == SP || code_.empty() || code_.size() <= barrier_) return -1;
        const std::size_t lo = std::max<std::size_t>(barrier_, code_.size() > kFwdWindow ? code_.size() - kFwdWindow : 0);
        std::uint32_t written_regs = 0;  // vector/general register numbers written after the candidate store
        for (std::size_t k = code_.size(); k-- > lo;) {
            const std::uint32_t i = code_[k];
            if ((i & 0xFFFFFFE0u) == (0x3D800000u | field)) {  // the store we look for
                const unsigned m = i & 31u;
                return ((written_regs >> m) & 1u) || ((written_regs >> basereg) & 1u) ? -1 : static_cast<int>(m);
            }
            if ((i & 0xFC000000u) == 0x94000000u || (i & 0xFFFFFC1Fu) == 0xD63F0000u) return -1;  // bl / blr
            if ((i & 0x0A000000u) == 0x08000000u) {  // load / store
                const bool pair = (i & 0x38000000u) == 0x28000000u;
                const bool load = (i >> 22) & 1u;
                const unsigned rt = i & 31u, rn = (i >> 5) & 31u;
                if (load) {
                    written_regs |= 1u << rt;
                    if (pair) written_regs |= 1u << ((i >> 10) & 31u);
                    continue;
                }
                if (rn == SP) continue;  // stack slots never alias the VU registers
                // A store through the same base with a provably different 16-byte range is fine; anything
                // else (other base, other addressing form) may hit the same bytes.
                if (rn != basereg || (i & 0x3B000000u) != 0x39000000u) return -1;
                const unsigned size = i >> 30, v = (i >> 26) & 1u, opc = (i >> 22) & 3u;
                const unsigned scale = v && size == 0 && (opc & 2u) ? 4u : size;
                const std::uint32_t at = ((i >> 10) & 0xFFFu) << scale, len = 1u << scale;
                if (at < disp + 16u && disp < at + len) return -1;
                continue;
            }
            written_regs |= 1u << (i & 31u);  // any other instruction: assume it writes register Rd
        }
        return -1;
    }

    // Vector ops that read lazy register locals directly.
    bool lazy_simd(std::uint32_t sub) {
        if (sub == 0x0C && !pinned_.empty()) {
            Reader peek = rd_;
            std::uint32_t lane[4] = {};
            for (int k = 0; k < 16; ++k) lane[k / 4] |= static_cast<std::uint32_t>(peek.byte()) << (8 * (k % 4));
            if (lane[0] == lane[1] && lane[0] == lane[2] && lane[0] == lane[3])
                for (const auto &[value, reg] : pinned_)
                    if (value == lane[0]) {
                        rd_ = peek;
                        if (!push(V128)) return true;
                        sync_lazy();
                        lz_[top()] = Lazy{kReg, reg, 0};
                        ++pinned_hits_;
                        return true;
                    }
        }
        static const struct {
            std::uint32_t sub, enc;
            bool swap;
        } kBin[] = {
            {0x37, 0x6EA08C00u, false}, {0x39, 0x4EA03400u, true},  {0x3B, 0x4EA03400u, false}, {0x3D, 0x4EA03C00u, true},
            {0x3F, 0x4EA03C00u, false}, {0x41, 0x4E20E400u, false}, {0x43, 0x6EA0E400u, true},  {0x44, 0x6EA0E400u, false},
            {0x45, 0x6E20E400u, true},  {0x46, 0x6E20E400u, false}, {0x4E, 0x4E201C00u, false}, {0x4F, 0x4E601C00u, false},
            {0x50, 0x4EA01C00u, false}, {0x51, 0x6E201C00u, false}, {0xAE, 0x4EA08400u, false}, {0xB1, 0x6EA08400u, false},
            {0xB6, 0x4EA06C00u, false}, {0xB7, 0x6EA06C00u, false}, {0xB8, 0x4EA06400u, false}, {0xE4, 0x4E20D400u, false}, {0xE5, 0x4EA0D400u, false},
            {0xE6, 0x6E20DC00u, false},
        };
        for (const auto &op : kBin) {
            if (op.sub != sub) continue;
            if (!need(2)) return true;
            const std::size_t a = top() - 1, b = top();
            const unsigned va = vsrc(a), vb = vsrc(b);
            const unsigned n = op.swap ? vb : va, m = op.swap ? va : vb;
            w(op.enc | m << 16 | n << 5 | vreg(a));
            stack_.pop_back();
            sync_lazy();
            set_slot(a);
            return true;
        }
        Reader &r = rd_;
        switch (sub) {
        case 0x0D: {  // i8x16.shuffle: dup for a lane broadcast, one-table tbl for one source
            std::uint8_t idx[16];
            for (auto &x : idx) x = r.byte();
            if (!need(2)) return true;
            const std::size_t a = top() - 1, b = top();
            const unsigned va = vsrc(a), vb = vsrc(b);
            bool only_a = true, only_b = true;
            for (std::uint8_t x : idx) {
                if (x >= 32) return fail("shuffle index");
                if (x < 16) only_b = false;
                else only_a = false;
            }
            unsigned src = only_a ? va : vb;
            bool single = only_a || only_b;
            if (!single && va == vb) {  // both inputs the same register: indices mod 16
                single = true;
                src = va;
            }
            if (single) {
                std::uint8_t m[16];
                for (int k = 0; k < 16; ++k) m[k] = idx[k] & 15u;
                bool broadcast = m[0] % 4 == 0;
                for (int k = 0; k < 16 && broadcast; ++k) broadcast = m[k] == m[k % 4] && m[k % 4] == m[0] + k % 4;
                if (broadcast) {
                    const unsigned lane = m[0] / 4u;
                    w(0x4E040400u | ((lane << 3) | 4u) << 16 | src << 5 | vreg(a));  // dup vd.4s, vn.s[lane]
                } else {
                    std::array<std::uint32_t, 4> lanes{};
                    for (int k = 0; k < 16; ++k) lanes[k / 4] |= static_cast<std::uint32_t>(m[k]) << (8 * (k % 4));
                    pool_refs_.push_back({code_.size(), lanes});
                    w(0x9C000000u | 2);                                // ldr q2, indices
                    w(0x4E000000u | 2 << 16 | src << 5 | vreg(a));    // tbl vd.16b, {vn.16b}, v2.16b
                }
            } else {
                std::array<std::uint32_t, 4> lanes{};
                for (int k = 0; k < 16; ++k) lanes[k / 4] |= static_cast<std::uint32_t>(idx[k]) << (8 * (k % 4));
                vmov(0, va);
                vmov(1, vb);
                pool_refs_.push_back({code_.size(), lanes});
                w(0x9C000000u | 2);
                w(0x4E002000u | 2 << 16 | 0 << 5 | vreg(a));  // tbl vd.16b, {v0, v1}, v2
            }
            stack_.pop_back();
            sync_lazy();
            set_slot(a);
            return true;
        }
        case 0x00: { r.u32(); return lazy_vmem(false, r.u32()); }
        case 0x0B: { r.u32(); return lazy_vmem(true, r.u32()); }
        case 0xA1: case 0xF8: case 0xFA: {
            if (!need(1)) return true;
            const std::uint32_t enc = sub == 0xA1 ? 0x6EA0B800u : sub == 0xF8 ? 0x4EA1B800u : 0x4E21D800u;
            w(enc | vsrc(top()) << 5 | vreg(top()));
            set_slot(top());
            return true;
        }
        case 0x11: {  // i32x4.splat
            if (!need(1)) return true;
            const unsigned n = isrc(top(), X17);
            w(0x4E040C00u | n << 5 | vreg(top()));
            stack_.back() = V128;
            set_slot(top());
            return true;
        }
        case 0x1B: {  // i32x4.extract_lane
            const std::uint32_t lane = r.byte();
            if (!need(1) || lane > 3) return fail("extract lane");
            w(0x0E003C00u | ((lane << 3) | 4u) << 16 | vsrc(top()) << 5 | ireg(top()));
            stack_.back() = I32;
            set_slot(top());
            return true;
        }
        case 0x1C: {  // i32x4.replace_lane: writes into the vector's slot
            const std::uint32_t lane = r.byte();
            if (!need(2) || lane > 3) return fail("replace lane");
            const std::size_t x = top(), v = top() - 1;
            materialize(v);
            w(0x4E001C00u | ((lane << 3) | 4u) << 16 | isrc(x, X17) << 5 | vreg(v));
            stack_.pop_back();
            sync_lazy();
            return true;
        }
        case 0x52: {  // bitselect(a, b, c) = (a & c) | (b & ~c)
            if (!need(3)) return true;
            const std::size_t c = top(), b = top() - 1, a = top() - 2;
            vmov(0, vsrc(c));
            w(0x6E601C00u | vsrc(b) << 16 | vsrc(a) << 5 | 0);  // bsl v0, va, vb
            vmov(vreg(a), 0);
            stack_.pop_back();
            stack_.pop_back();
            sync_lazy();
            set_slot(a);
            return true;
        }
        default: return false;
        }
    }

    // Number of operands an instruction reads from the stack in instruction_slots; -1 = all
    // (control flow, calls, unknown): those are materialised before it runs.
    int slot_reads(std::uint8_t op) {
        switch (op) {
        case 0x1A: return 0;
        case 0x20: return 0;
        case 0x21: case 0x22: return 1;
        case 0x1B: return 3;
        case 0x28: case 0x29: case 0x2C: case 0x2D: case 0x2E: case 0x2F: return 1;
        case 0x36: case 0x37: case 0x3A: case 0x3B: return 2;
        case 0x41: case 0x42: return 0;
        case 0x45: case 0x9F: case 0xA7: case 0xAC: case 0xAD: case 0xBD: case 0xBF: case 0xC0: case 0xC1: return 1;
        case 0xA3: return 2;
        case 0xFD: {
            Reader peek = rd_;
            const std::uint32_t sub = peek.u32();
            switch (sub) {
            case 0x00: case 0x11: case 0x1B: case 0xA1: case 0xF8: case 0xFA: return 1;
            case 0x0C: return 0;
            case 0x52: return 3;
            default: return 2;
            }
        }
        default:
            if (op >= 0x46 && op <= 0x8A) return 2;
            return -1;
        }
    }

    bool instruction(std::uint8_t op) {
        Reader &r = rd_;
        if (dead_) {
            const bool ok = instruction_slots(op);
            sync_lazy();
            return ok;
        }
        // NZCV only survives into the instructions that consume it.
        const bool cond_top = !stack_.empty() && lz_[top()].kind == kCond;
        if (!(cond_top && (op == 0x04 || op == 0x0D || op == 0x1B || op == 0x45))) materialize_cond();

        switch (op) {
        case 0x20: case 0x21: case 0x22: {
            Reader peek = r;
            const std::uint32_t l = peek.u32();
            if (l >= local_type_.size()) return fail("local index");
            const int lr = local_reg_[l];
            const Type t = local_type_[l];
            if (op == 0x20 && l < 2 && const_param_[l]) {
                r.u32();
                if (!push(t)) return false;
                sync_lazy();
                lz_[top()] = Lazy{kImm, 0, kParamOffset[l]};
                return true;
            }
            if (op == 0x20) {
                if (lr < 0) break;  // frame local: slot load
                r.u32();
                if (!push(t)) return false;
                sync_lazy();
                lz_[top()] = Lazy{kReg, static_cast<unsigned>(lr), 0};
                return true;
            }
            if (!need(1)) return false;
            if (lr < 0) {
                materialize(top());
                break;
            }
            r.u32();
            const auto reg = static_cast<unsigned>(lr);
            const Lazy e = lz_[top()];
            const bool self = e.kind == kReg && e.reg == reg;
            bool older_reads = false;
            for (std::size_t d = 0; d < top(); ++d) older_reads = older_reads || (lz_[d].kind == kReg && lz_[d].reg == reg);
            if (!self && e.kind == kSlot && !older_reads &&
                retarget_last(is_vec(t) ? vreg(top()) : ireg(top()), reg, is_vec(t))) {
                // the value was computed straight into the local's register
            } else if (!self) {
                materialize_reg(reg, top());  // older reads of the local keep the old value
                if (is_vec(t)) vmov(reg, vsrc(top()));
                else if (e.kind == kImm) {
                    if (t == I64) movx(reg, e.imm);
                    else movw(reg, static_cast<std::uint32_t>(e.imm));
                } else {
                    w(0xAA0003E0u | isrc(top(), X17) << 16 | reg);  // mov xl, x
                }
            }
            if (op == 0x21) {
                stack_.pop_back();
                sync_lazy();
            } else {
                lz_[top()] = Lazy{kReg, reg, 0};
            }
            return true;
        }
        case 0x41: {
            const auto v = static_cast<std::uint32_t>(r.sleb(32));
            if (!push(I32)) return false;
            sync_lazy();
            lz_[top()] = Lazy{kImm, 0, v};
            return true;
        }
        case 0x42: {
            const auto v = static_cast<std::uint64_t>(r.sleb(64));
            if (!push(I64)) return false;
            sync_lazy();
            lz_[top()] = Lazy{kImm, 0, v};
            return true;
        }
        case 0x45: {  // eqz
            if (!need(1)) return false;
            Lazy &e = lz_[top()];
            if (e.kind == kCond) {
                e.reg ^= 1u;
                return true;
            }
            if (e.kind == kImm) {
                e.imm = (stack_[top()] == I64 ? e.imm : (e.imm & 0xFFFFFFFFull)) == 0;
                stack_.back() = I32;
                return true;
            }
            const bool wide = stack_[top()] == I64;
            w((wide ? 0xF100001Fu : 0x7100001Fu) | isrc(top()) << 5);  // cmp x, #0
            stack_.back() = I32;
            lz_[top()] = Lazy{kCond, EQ, 0};
            return true;
        }
        case 0x04: {  // if
            if (!need(1)) return false;
            const Lazy e = lz_[top()];
            if (e.kind != kCond && e.kind != kReg) break;
            if (r.byte() != 0x40) return fail("if with a result");
            for (std::size_t d = 0; d + 1 < stack_.size(); ++d) materialize(d);
            stack_.pop_back();
            sync_lazy();
            Ctrl c{Ctrl::If, stack_.size()};
            c.to_end.push_back({code_.size(), kCond19});
            if (e.kind == kCond) w(0x54000000u | (e.reg ^ 1u));  // b.!cond end
            else w(0x34000000u | e.reg);                         // cbz wl, end
            ctrl_.push_back(std::move(c));
            return true;
        }
        case 0x0D: {  // br_if
            if (!need(1)) return false;
            const Lazy e = lz_[top()];
            if (e.kind != kCond && e.kind != kReg) break;
            Reader peek = r;
            const std::uint32_t depth = peek.u32();
            if (ctrl_[ctrl_.size() - 1 - depth].kind == Ctrl::Function) return fail("br_if to the function");
            r.u32();
            for (std::size_t d = 0; d + 1 < stack_.size(); ++d) materialize(d);
            stack_.pop_back();
            sync_lazy();
            if (e.kind == kCond) branch_to(depth, 0x54000000u | e.reg, kCond19);  // b.cond
            else branch_to(depth, 0x35000000u | e.reg, kCond19);                  // cbnz
            return true;
        }
        case 0x1B: {  // select
            if (!need(3)) return false;
            const std::size_t c = top(), b = top() - 1, a = top() - 2;
            unsigned cond = NE;
            if (lz_[c].kind == kCond) cond = lz_[c].reg;
            else w(0x7100001Fu | isrc_nz(c, X17) << 5);  // cmp c, #0
            if (is_vec(stack_[a])) {
                w(0x5A9F03E0u | (cond ^ 1u) << 12 | 0);  // csetm w0, cond
                w(0x4E040C00u | 0 << 5 | 0);             // dup v0.4s, w0
                w(0x6E601C00u | vsrc(b) << 16 | vsrc(a) << 5 | 0);
                vmov(vreg(a), 0);
            } else {
                const unsigned va = isrc(a, X16), vb = isrc(b, X17);
                w((stack_[a] == I64 ? 0x9A800000u : 0x1A800000u) | vb << 16 | cond << 12 | va << 5 | ireg(a));
            }
            stack_.pop_back();
            stack_.pop_back();
            sync_lazy();
            set_slot(a);
            return true;
        }
        case 0x28: case 0x29: case 0x2C: case 0x2D: case 0x2E: case 0x2F: {
            r.u32();
            const std::uint32_t off = r.u32();
            switch (op) {
            case 0x28: return lazy_mem(false, 2, false, off);
            case 0x29: return lazy_mem(false, 3, false, off);
            case 0x2C: return lazy_mem(false, 0, true, off);
            case 0x2D: return lazy_mem(false, 0, false, off);
            case 0x2E: return lazy_mem(false, 1, true, off);
            default: return lazy_mem(false, 1, false, off);
            }
        }
        case 0x36: case 0x37: case 0x3A: case 0x3B: {
            r.u32();
            const std::uint32_t off = r.u32();
            return lazy_mem(true, op == 0x36 ? 2 : op == 0x37 ? 3 : op == 0x3A ? 0 : 1, false, off);
        }
        case 0xA7: case 0xAD: {  // wrap / extend_u: 32-bit move (constants fold)
            if (!need(1)) return false;
            Lazy &e = lz_[top()];
            if (e.kind == kImm) {
                e.imm &= 0xFFFFFFFFull;
                stack_.back() = op == 0xA7 ? I32 : I64;
                return true;
            }
            const unsigned n = isrc(top());
            w(0x2A0003E0u | n << 16 | ireg(top()));
            stack_.back() = op == 0xA7 ? I32 : I64;
            set_slot(top());
            return true;
        }
        case 0xFD: {
            Reader save = r;
            const std::uint32_t sub = r.u32();
            if (lazy_simd(sub)) return !*error_;
            r = save;
            break;
        }
        default:
            if (lazy_binary(op)) return !*error_;
            if (lazy_compare(op)) return !*error_;
            break;
        }

        // Generic path: materialise what the slot translation reads, then run it.
        const int n = slot_reads(op);
        const std::size_t h0 = stack_.size();
        if (n < 0) materialize_all();
        else
            for (std::size_t k = 0; k < static_cast<std::size_t>(n) && k < h0; ++k) materialize(h0 - 1 - k);
        const bool ok = instruction_slots(op);
        const std::size_t lo = n < 0 ? 0 : (h0 >= static_cast<std::size_t>(n) ? h0 - static_cast<std::size_t>(n) : 0);
        sync_lazy();
        for (std::size_t d = lo; d < stack_.size(); ++d) set_slot(d);
        return ok;
    }

    // The slot-based translation of one instruction: operands in their stack slot registers.
    bool instruction_slots(std::uint8_t op) {
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
            barrier_ = code_.size();
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
            barrier_ = code_.size();  // table words are data, not instructions
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
            const int lr = local_reg_[l];
            if (op == 0x20) {
                if (!push(t)) return false;
                if (lr < 0) frame_ld(t, reg_of(top()), local_off_[l]);
                else if (is_vec(t)) vmov(vreg(top()), static_cast<unsigned>(lr));
                else w(0xAA0003E0u | static_cast<unsigned>(lr) << 16 | ireg(top()));  // mov x, xl
                return true;
            }
            if (!need(1)) return false;
            if (lr < 0) frame_st(t, reg_of(top()), local_off_[l]);
            else if (is_vec(t)) vmov(static_cast<unsigned>(lr), vreg(top()));
            else w(0xAA0003E0u | ireg(top()) << 16 | static_cast<unsigned>(lr));  // mov xl, x
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
            if (v[0] == v[1] && v[0] == v[2] && v[0] == v[3] && g_movi && splat_imm(vreg(top()), v[0])) return true;
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
        case 0xB7: return binary_vec(0x6EA06C00u);               // i32x4.min_u
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
    std::vector<int> local_reg_;            // register of a local, -1 = in the frame
    std::vector<std::uint32_t> vec_locals_;  // locals held in vector registers
    std::vector<std::uint32_t> scratch_int_locals_;  // integer locals in caller-saved registers
    bool const_param_[2] = {};
    std::uint32_t locals_size_ = 0, spill_off_ = 0, args_off_ = 0, frame_ = 0;
    std::vector<std::uint32_t> code_;
    std::vector<std::pair<std::size_t, std::array<std::uint32_t, 4>>> pool_refs_;
    std::vector<Type> stack_;
    std::vector<Ctrl> ctrl_;
    bool dead_ = false;
    std::size_t barrier_ = 0;
    std::vector<std::pair<std::size_t, std::uint32_t>> stub_calls_;
    static constexpr unsigned kMaxPinned = 6;
    std::vector<std::pair<std::uint32_t, unsigned>> pinned_;  // splat value, vector register
    unsigned vec_regs_avail_ = sizeof kVecLocalRegs / sizeof kVecLocalRegs[0];  // bl positions + flag_stub kind     // code index of the last branch target: retarget_last never crosses it
    bool range_error_ = false;
public:
    std::size_t retargeted_ = 0;  // local.set movs saved (stats)
    std::size_t fwd_hits_ = 0;  // vector reloads replaced by a register copy (stats)
    std::size_t pinned_hits_ = 0;  // v128.const uses served by a pinned register (stats)
    std::size_t pinned_count() const { return pinned_.size(); }
    std::size_t max_depth_ = 0;
    std::size_t locals_used_int_ = 0, locals_used_vec_ = 0;
private:
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
    kParamOffset[0] = a - low;
    kParamOffset[1] = b - low;
    g_base_ok = high - low < (std::uintptr_t{1} << 31);
    std::fprintf(stderr, "[vu-jit] ARM64 VU1 JIT %s (span %zu KB)\n", g_base_ok ? "on" : "off: data too far apart",
                 static_cast<std::size_t>((high - low) >> 10));
    return g_base_ok;
}

}

// Flush-to-zero while VU1 programs run (vu_micro.cpp run_entry sets FPCR.FZ when this is on): lets the
// Translator clamp arithmetic results with 2 instructions instead of 6 (Layout::ftz). PS2_VU_JIT_FTZ=0: off.
bool a64_ftz() {
    static const bool on = [] { const char *v = std::getenv("PS2_VU_JIT_FTZ"); return !v || v[0] != '0'; }();
    return on;
}

// For tests/vu_jit_a64_test.cpp: the data base, the flag pipe and the import functions.
std::uintptr_t a64_base() { return init_base() ? g_base : 0; }
void *a64_flags() { return &g_fl; }
const void *a64_import(std::uint32_t i) { return i < kImports ? kImportFns[i].fn : nullptr; }

ProgramFn compile_a64(const std::uint8_t *code, std::uint32_t entry) {
    if (!init_base()) return nullptr;
    std::vector<std::uint8_t> module;
    const Layout layout{static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&g_fl) - g_base), a64_ftz()};
    if (!translate(code, entry, layout, module)) return nullptr;
    Compiler c;
    std::vector<std::uint32_t> words;
    if (!c.compile(module, words)) {
        static unsigned reported = 0;
        if (reported++ < 10) std::fprintf(stderr, "[vu-jit] program at 0x%04X not compiled: %s\n", entry, c.error());
        return nullptr;
    }
    std::uint32_t *fn = place(words);
    // PS2_VU_JIT_DUMP=<dir>: raw ARM64 code of every compiled program (vu1_<entry>_<address>.bin).
    if (const char *dir = std::getenv("PS2_VU_JIT_DUMP")) {
        std::fprintf(stderr, "[vu-jit] %04X: %zu words, stack depth %zu, locals used %zu int / %zu vec, %zu movs saved, %zu consts pinned (%zu uses), %zu reloads forwarded\n", entry, words.size(),
                     c.max_depth_, c.locals_used_int_, c.locals_used_vec_, c.retargeted_, c.pinned_count(), c.pinned_hits_, c.fwd_hits_);
        char name[512];
        std::snprintf(name, sizeof name, "%s/vu1_%04X_%llx.bin", dir, entry, static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(fn)));
        if (std::FILE *f = std::fopen(name, "wb")) {
            std::fwrite(words.data(), 4, words.size(), f);
            std::fclose(f);
        }
    }
    return reinterpret_cast<ProgramFn>(fn);
}

}

#endif

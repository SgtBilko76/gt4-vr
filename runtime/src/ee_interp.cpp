// EE (R5900) interpreter: runs the game's main program straight from guest memory, without the C++
// that eerecomp generates from the game's ELF. It mirrors recomp/codegen.cpp rule for rule (call and
// return protocol, delay slots, budget, preemption, probes), so the runtime, the kernel and the host
// hooks see the same behaviour as with translated code.
//
// Protocol (same as generated units): a call runs the callee in a nested C++ frame (ps2::call), which
// returns with c.pc = the return address; anything else in c.pc (a thread switch, kUnwind, an
// exception) makes every frame return to the main loop, which dispatches c.pc again. `jr $ra` sets
// c.pc and returns. Jumps and branches stay in this frame.
#include <vector>
#include <algorithm>
#include <unordered_map>
#include "ee_core.hpp"
#include "ps2/system.hpp"

#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ps2::ee {

namespace {

using recomp::Flow;
using recomp::Insn;
using enum recomp::Op;

// Decoded instructions, direct mapped by address; an entry is valid while its word is unchanged
// (code the game rewrites or loads later decodes again).
struct Slot {
    std::uint32_t pc = 1;  // never a valid pc
    std::uint32_t word = 0;
    Insn insn;
};
constexpr std::uint32_t kSlots = 1u << 16;
Slot g_slots[kSlots];

inline const Insn &fetch_cached(std::uint32_t pc) {
    const std::uint32_t word = ld32(pc);
    Slot &s = g_slots[(pc >> 2) & (kSlots - 1)];
    if (s.pc != pc || s.word != word) [[unlikely]] {
        s.pc = pc;
        s.word = word;
        s.insn = recomp::decode(word, pc);
    }
    return s.insn;
}

inline std::uint32_t W(const Cpu &c, unsigned r) { return c.r[r].w[0]; }
inline std::uint64_t D(const Cpu &c, unsigned r) { return c.r[r].d[0]; }
inline std::int64_t SD(const Cpu &c, unsigned r) { return c.r[r].sd[0]; }
inline void setw(Cpu &c, unsigned r, std::uint32_t v) { if (r) set32(c.r[r], v); }
inline void setd(Cpu &c, unsigned r, std::uint64_t v) { if (r) c.r[r].d[0] = v; }
inline std::uint32_t addr(const Cpu &c, const Insn &i) { return W(c, i.rs()) + static_cast<std::uint32_t>(i.simm()); }

bool condition_impl(Cpu &c, const Insn &i) {
    switch (i.op) {
    case Beq: case Beql: return D(c, i.rs()) == D(c, i.rt());
    case Bne: case Bnel: return D(c, i.rs()) != D(c, i.rt());
    case Blez: case Blezl: return SD(c, i.rs()) <= 0;
    case Bgtz: case Bgtzl: return SD(c, i.rs()) > 0;
    case Bltz: case Bltzl: case Bltzal: case Bltzall: return SD(c, i.rs()) < 0;
    case Bgez: case Bgezl: case Bgezal: case Bgezall: return SD(c, i.rs()) >= 0;
    case Bc0f: case Bc0fl: return !cpcond0(c);
    case Bc0t: case Bc0tl: return cpcond0(c);
    case Bc1f: case Bc1fl: return !(c.fcr31 & kFpuCondition);
    case Bc1t: case Bc1tl: return (c.fcr31 & kFpuCondition) != 0;
    case Bc2f: case Bc2fl: return !vu0_busy(c);
    case Bc2t: case Bc2tl: return vu0_busy(c);
    default: return false;
    }
}

// One instruction without control flow (codegen.cpp simple()). False = the CPU stopped.
bool simple_impl(Cpu &c, const Insn &i) {
    const unsigned rs = i.rs(), rt = i.rt(), rd = i.rd(), sa = i.sa();
    const unsigned fsf = (i.word >> 21) & 3u, ftf = (i.word >> 23) & 3u;
    Reg128 scratch;
    Reg128 &dreg = rd ? c.r[rd] : scratch;
    auto &v = c.vu0;
    switch (i.op) {
    case Sll: if (i.word) setw(c, rd, W(c, rt) << sa); break;
    case Srl: setw(c, rd, W(c, rt) >> sa); break;
    case Sra: setw(c, rd, static_cast<std::uint32_t>(static_cast<std::int32_t>(W(c, rt)) >> sa)); break;
    case Sllv: setw(c, rd, W(c, rt) << (W(c, rs) & 31u)); break;
    case Srlv: setw(c, rd, W(c, rt) >> (W(c, rs) & 31u)); break;
    case Srav: setw(c, rd, static_cast<std::uint32_t>(static_cast<std::int32_t>(W(c, rt)) >> (W(c, rs) & 31u))); break;
    case Movz: if (rd && D(c, rt) == 0) c.r[rd].d[0] = D(c, rs); break;
    case Movn: if (rd && D(c, rt) != 0) c.r[rd].d[0] = D(c, rs); break;
    case Break: breakpoint(c, i.pc); break;
    case Cache: cache_op(rt); break;
    case Sync: case Pref: break;
    case Mfhi: setd(c, rd, c.hi.d[0]); break;
    case Mflo: setd(c, rd, c.lo.d[0]); break;
    case Mfhi1: setd(c, rd, c.hi.d[1]); break;
    case Mflo1: setd(c, rd, c.lo.d[1]); break;
    case Mthi: c.hi.d[0] = D(c, rs); break;
    case Mtlo: c.lo.d[0] = D(c, rs); break;
    case Mthi1: c.hi.d[1] = D(c, rs); break;
    case Mtlo1: c.lo.d[1] = D(c, rs); break;
    case Dsllv: setd(c, rd, D(c, rt) << (W(c, rs) & 63u)); break;
    case Dsrlv: setd(c, rd, D(c, rt) >> (W(c, rs) & 63u)); break;
    case Dsrav: setd(c, rd, static_cast<std::uint64_t>(SD(c, rt) >> (W(c, rs) & 63u))); break;
    case Mult: mmi::mult(c, 0, W(c, rs), W(c, rt)); setd(c, rd, c.lo.d[0]); break;
    case Multu: mmi::multu(c, 0, W(c, rs), W(c, rt)); setd(c, rd, c.lo.d[0]); break;
    case Madd: mmi::madd(c, 0, W(c, rs), W(c, rt)); setd(c, rd, c.lo.d[0]); break;
    case Maddu: mmi::maddu(c, 0, W(c, rs), W(c, rt)); setd(c, rd, c.lo.d[0]); break;
    case Mult1: mmi::mult(c, 1, W(c, rs), W(c, rt)); setd(c, rd, c.lo.d[1]); break;
    case Multu1: mmi::multu(c, 1, W(c, rs), W(c, rt)); setd(c, rd, c.lo.d[1]); break;
    case Madd1: mmi::madd(c, 1, W(c, rs), W(c, rt)); setd(c, rd, c.lo.d[1]); break;
    case Maddu1: mmi::maddu(c, 1, W(c, rs), W(c, rt)); setd(c, rd, c.lo.d[1]); break;
    case Div: mmi::div(c, 0, W(c, rs), W(c, rt)); break;
    case Divu: mmi::divu(c, 0, W(c, rs), W(c, rt)); break;
    case Div1: mmi::div(c, 1, W(c, rs), W(c, rt)); break;
    case Divu1: mmi::divu(c, 1, W(c, rs), W(c, rt)); break;
    case Add: case Addu: setw(c, rd, W(c, rs) + W(c, rt)); break;
    case Sub: case Subu: setw(c, rd, W(c, rs) - W(c, rt)); break;
    case And: setd(c, rd, D(c, rs) & D(c, rt)); break;
    case Or: setd(c, rd, D(c, rs) | D(c, rt)); break;
    case Xor: setd(c, rd, D(c, rs) ^ D(c, rt)); break;
    case Nor: setd(c, rd, ~(D(c, rs) | D(c, rt))); break;
    case Mfsa: setd(c, rd, c.sa); break;
    case Mtsa: c.sa = W(c, rs) & 15u; break;
    case Mtsab: c.sa = (W(c, rs) ^ i.uimm()) & 15u; break;
    case Mtsah: c.sa = ((W(c, rs) ^ i.uimm()) & 7u) * 2u; break;
    case Slt: setd(c, rd, SD(c, rs) < SD(c, rt) ? 1ull : 0ull); break;
    case Sltu: setd(c, rd, D(c, rs) < D(c, rt) ? 1ull : 0ull); break;
    case Dadd: case Daddu: setd(c, rd, D(c, rs) + D(c, rt)); break;
    case Dsub: case Dsubu: setd(c, rd, D(c, rs) - D(c, rt)); break;
    case Tge: if (SD(c, rs) >= SD(c, rt)) trap(c, i.pc); break;
    case Tgeu: if (D(c, rs) >= D(c, rt)) trap(c, i.pc); break;
    case Tlt: if (SD(c, rs) < SD(c, rt)) trap(c, i.pc); break;
    case Tltu: if (D(c, rs) < D(c, rt)) trap(c, i.pc); break;
    case Teq: if (D(c, rs) == D(c, rt)) trap(c, i.pc); break;
    case Tne: if (D(c, rs) != D(c, rt)) trap(c, i.pc); break;
    case Tgei: if (SD(c, rs) >= i.simm()) trap(c, i.pc); break;
    case Tgeiu: if (D(c, rs) >= static_cast<std::uint64_t>(static_cast<std::int64_t>(i.simm()))) trap(c, i.pc); break;
    case Tlti: if (SD(c, rs) < i.simm()) trap(c, i.pc); break;
    case Tltiu: if (D(c, rs) < static_cast<std::uint64_t>(static_cast<std::int64_t>(i.simm()))) trap(c, i.pc); break;
    case Teqi: if (SD(c, rs) == i.simm()) trap(c, i.pc); break;
    case Tnei: if (SD(c, rs) != i.simm()) trap(c, i.pc); break;
    case Dsll: setd(c, rd, D(c, rt) << sa); break;
    case Dsrl: setd(c, rd, D(c, rt) >> sa); break;
    case Dsra: setd(c, rd, static_cast<std::uint64_t>(SD(c, rt) >> sa)); break;
    case Dsll32: setd(c, rd, D(c, rt) << (sa + 32)); break;
    case Dsrl32: setd(c, rd, D(c, rt) >> (sa + 32)); break;
    case Dsra32: setd(c, rd, static_cast<std::uint64_t>(SD(c, rt) >> (sa + 32))); break;
    case Addi: case Addiu: setw(c, rt, W(c, rs) + static_cast<std::uint32_t>(i.simm())); break;
    case Slti: setd(c, rt, SD(c, rs) < i.simm() ? 1ull : 0ull); break;
    case Sltiu: setd(c, rt, D(c, rs) < static_cast<std::uint64_t>(static_cast<std::int64_t>(i.simm())) ? 1ull : 0ull); break;
    case Andi: setd(c, rt, D(c, rs) & i.uimm()); break;
    case Ori: setd(c, rt, D(c, rs) | i.uimm()); break;
    case Xori: setd(c, rt, D(c, rs) ^ i.uimm()); break;
    case Lui: if (rt) c.r[rt].sd[0] = static_cast<std::int32_t>(i.uimm() << 16); break;
    case Daddi: case Daddiu: setd(c, rt, D(c, rs) + static_cast<std::uint64_t>(static_cast<std::int64_t>(i.simm()))); break;
    case Ldl: if (rt) ldl(c.r[rt], addr(c, i)); break;
    case Ldr: if (rt) ldr(c.r[rt], addr(c, i)); break;
    case Lwl: if (rt) lwl(c.r[rt], addr(c, i)); break;
    case Lwr: if (rt) lwr(c.r[rt], addr(c, i)); break;
    case Sdl: sdl(c.r[rt], addr(c, i)); break;
    case Sdr: sdr(c.r[rt], addr(c, i)); break;
    case Swl: swl(c.r[rt], addr(c, i)); break;
    case Swr: swr(c.r[rt], addr(c, i)); break;
    case Lq: ld128(addr(c, i) & ~15u, rt ? c.r[rt] : scratch); break;
    case Sq: st128(addr(c, i) & ~15u, c.r[rt]); break;
    case Lb: { const std::uint8_t x = ld8(addr(c, i)); if (rt) c.r[rt].sd[0] = static_cast<std::int8_t>(x); break; }
    case Lbu: { const std::uint8_t x = ld8(addr(c, i)); if (rt) c.r[rt].d[0] = x; break; }
    case Lh: { const std::uint16_t x = ld16(addr(c, i)); if (rt) c.r[rt].sd[0] = static_cast<std::int16_t>(x); break; }
    case Lhu: { const std::uint16_t x = ld16(addr(c, i)); if (rt) c.r[rt].d[0] = x; break; }
    case Lw: { const std::uint32_t x = ld32(addr(c, i)); if (rt) set32(c.r[rt], x); break; }
    case Lwu: { const std::uint32_t x = ld32(addr(c, i)); if (rt) c.r[rt].d[0] = x; break; }
    case Ld: { const std::uint64_t x = ld64(addr(c, i)); if (rt) c.r[rt].d[0] = x; break; }
    case Sb: st8(addr(c, i), static_cast<std::uint8_t>(W(c, rt))); break;
    case Sh: st16(addr(c, i), static_cast<std::uint16_t>(W(c, rt))); break;
    case Sw: st32(addr(c, i), W(c, rt)); break;
    case Sd: st64(addr(c, i), D(c, rt)); break;
    case Lwc1: c.f[i.ft()] = ld32(addr(c, i)); break;
    case Swc1: st32(addr(c, i), c.f[i.ft()]); break;
    case Lqc2: ld128(addr(c, i) & ~15u, i.ft() ? v.vf[i.ft()] : scratch); break;
    case Sqc2: st128(addr(c, i) & ~15u, v.vf[i.ft()]); break;

    case Plzcw: if (rd) mmi::plzcw(c.r[rd], c.r[rs]); break;
    case Pmfhl: if (rd) mmi::pmfhl(c, c.r[rd], sa); break;
    case Pmthl: mmi::pmthl(c, c.r[rs]); break;
    case Psllh: if (rd) mmi::psllh(c.r[rd], c.r[rt], sa); break;
    case Psrlh: if (rd) mmi::psrlh(c.r[rd], c.r[rt], sa); break;
    case Psrah: if (rd) mmi::psrah(c.r[rd], c.r[rt], sa); break;
    case Psllw: if (rd) mmi::psllw(c.r[rd], c.r[rt], sa); break;
    case Psrlw: if (rd) mmi::psrlw(c.r[rd], c.r[rt], sa); break;
    case Psraw: if (rd) mmi::psraw(c.r[rd], c.r[rt], sa); break;
#define MMI3(OP, FN) case OP: if (rd) mmi::FN(c.r[rd], c.r[rs], c.r[rt]); break;
#define MMI2(OP, FN) case OP: if (rd) mmi::FN(c.r[rd], c.r[rt]); break;
    MMI3(Paddw, paddw) MMI3(Psubw, psubw) MMI3(Pcgtw, pcgtw) MMI3(Pmaxw, pmaxw)
    MMI3(Paddh, paddh) MMI3(Psubh, psubh) MMI3(Pcgth, pcgth) MMI3(Pmaxh, pmaxh)
    MMI3(Paddb, paddb) MMI3(Psubb, psubb) MMI3(Pcgtb, pcgtb)
    MMI3(Paddsw, paddsw) MMI3(Psubsw, psubsw) MMI3(Pextlw, pextlw) MMI3(Ppacw, ppacw)
    MMI3(Paddsh, paddsh) MMI3(Psubsh, psubsh) MMI3(Pextlh, pextlh) MMI3(Ppach, ppach)
    MMI3(Paddsb, paddsb) MMI3(Psubsb, psubsb) MMI3(Pextlb, pextlb) MMI3(Ppacb, ppacb)
    MMI2(Pext5, pext5) MMI2(Ppac5, ppac5)
    MMI2(Pabsw, pabsw) MMI3(Pceqw, pceqw) MMI3(Pminw, pminw) MMI3(Padsbh, padsbh)
    MMI2(Pabsh, pabsh) MMI3(Pceqh, pceqh) MMI3(Pminh, pminh) MMI3(Pceqb, pceqb)
    MMI3(Padduw, padduw) MMI3(Psubuw, psubuw) MMI3(Pextuw, pextuw)
    MMI3(Padduh, padduh) MMI3(Psubuh, psubuh) MMI3(Pextuh, pextuh)
    MMI3(Paddub, paddub) MMI3(Psubub, psubub) MMI3(Pextub, pextub)
    MMI3(Psllvw, psllvw) MMI3(Psrlvw, psrlvw) MMI3(Psravw, psravw)
    MMI3(Pinth, pinth) MMI3(Pinteh, pinteh) MMI3(Pcpyld, pcpyld) MMI3(Pcpyud, pcpyud)
    MMI3(Pand, pand) MMI3(Pxor, pxor) MMI3(Por, por) MMI3(Pnor, pnor)
    MMI2(Pexeh, pexeh) MMI2(Prevh, prevh) MMI2(Pexew, pexew) MMI2(Prot3w, prot3w)
    MMI2(Pexch, pexch) MMI2(Pcpyh, pcpyh) MMI2(Pexcw, pexcw)
#undef MMI3
#undef MMI2
    case Qfsrv: if (rd) mmi::qfsrv(c.r[rd], c.r[rs], c.r[rt], c.sa); break;
    case Pmaddw: mmi::pmaddw(c, dreg, c.r[rs], c.r[rt], false); break;
    case Pmsubw: mmi::pmaddw(c, dreg, c.r[rs], c.r[rt], true); break;
    case Pmadduw: mmi::pmadduw(c, dreg, c.r[rs], c.r[rt]); break;
    case Pmultw: mmi::pmultw(c, dreg, c.r[rs], c.r[rt]); break;
    case Pmultuw: mmi::pmultuw(c, dreg, c.r[rs], c.r[rt]); break;
    case Pmulth: mmi::pmulth(c, dreg, c.r[rs], c.r[rt], 0); break;
    case Pmaddh: mmi::pmulth(c, dreg, c.r[rs], c.r[rt], 1); break;
    case Pmsubh: mmi::pmulth(c, dreg, c.r[rs], c.r[rt], 2); break;
    case Phmadh: mmi::phmadh(c, dreg, c.r[rs], c.r[rt], false); break;
    case Phmsbh: mmi::phmadh(c, dreg, c.r[rs], c.r[rt], true); break;
    case Pdivw: mmi::pdivw(c, c.r[rs], c.r[rt]); break;
    case Pdivuw: mmi::pdivuw(c, c.r[rs], c.r[rt]); break;
    case Pdivbw: mmi::pdivbw(c, c.r[rs], c.r[rt]); break;
    case Pmfhi: if (rd) c.r[rd] = c.hi; break;
    case Pmflo: if (rd) c.r[rd] = c.lo; break;
    case Pmthi: c.hi = c.r[rs]; break;
    case Pmtlo: c.lo = c.r[rs]; break;

    case Mfc0: setw(c, rt, mfc0(c, rd, i.word & 7u)); break;
    case Mtc0: mtc0(c, rd, W(c, rt)); break;
    case Ei: set_interrupts(c, true); break;
    case Di: set_interrupts(c, false); break;
    case Tlbr: case Tlbwi: case Tlbwr: case Tlbp: stop(c, i.pc, recomp::name(i.op)); return false;

    case Mfc1: setw(c, rt, c.f[i.fs()]); break;
    case Mtc1: c.f[i.fs()] = W(c, rt); break;
    case Cfc1: setw(c, rt, cfc1(c, i.fs())); break;
    case Ctc1: ctc1(c, i.fs(), W(c, rt)); break;
    case AddS: c.f[i.fd()] = fp::add(c.f[i.fs()], c.f[i.ft()]); break;
    case SubS: c.f[i.fd()] = fp::sub(c.f[i.fs()], c.f[i.ft()]); break;
    case MulS: c.f[i.fd()] = fp::fpu_mul(c.f[i.fs()], c.f[i.ft()]); break;
    case DivS: fpu_div(c, i.fd(), c.f[i.fs()], c.f[i.ft()]); break;
    case SqrtS: c.f[i.fd()] = fp::sqrt(c.f[i.ft()]); break;
    case AbsS: c.f[i.fd()] = c.f[i.fs()] & fp::kAbs; break;
    case MovS: c.f[i.fd()] = c.f[i.fs()]; break;
    case NegS: c.f[i.fd()] = c.f[i.fs()] ^ fp::kSign; break;
    case RsqrtS: fpu_rsqrt(c, i.fd(), c.f[i.fs()], c.f[i.ft()]); break;
    case AddaS: c.facc = fp::add(c.f[i.fs()], c.f[i.ft()]); break;
    case SubaS: c.facc = fp::sub(c.f[i.fs()], c.f[i.ft()]); break;
    case MulaS: c.facc = fp::fpu_mul(c.f[i.fs()], c.f[i.ft()]); break;
    case MaddS: c.f[i.fd()] = fp::add(c.facc, fp::fpu_mul(c.f[i.fs()], c.f[i.ft()])); break;
    case MsubS: c.f[i.fd()] = fp::sub(c.facc, fp::fpu_mul(c.f[i.fs()], c.f[i.ft()])); break;
    case MaddaS: c.facc = fp::add(c.facc, fp::fpu_mul(c.f[i.fs()], c.f[i.ft()])); break;
    case MsubaS: c.facc = fp::sub(c.facc, fp::fpu_mul(c.f[i.fs()], c.f[i.ft()])); break;
    case CvtWS: c.f[i.fd()] = fp::cvt_w_s(c.f[i.fs()]); break;
    case CvtSW: c.f[i.fd()] = fp::cvt_s_w(c.f[i.fs()]); break;
    case MaxS: c.f[i.fd()] = fp::max(c.f[i.fs()], c.f[i.ft()]); break;
    case MinS: c.f[i.fd()] = fp::min(c.f[i.fs()], c.f[i.ft()]); break;
    case CF: fpu_compare(c, false); break;
    case CEq: fpu_compare(c, fp::key(c.f[i.fs()]) == fp::key(c.f[i.ft()])); break;
    case CLt: fpu_compare(c, fp::key(c.f[i.fs()]) < fp::key(c.f[i.ft()])); break;
    case CLe: fpu_compare(c, fp::key(c.f[i.fs()]) <= fp::key(c.f[i.ft()])); break;

    case Qmfc2:
        if (i.word & 1u) vu0_wait(c);
        if (rt) c.r[rt] = v.vf[i.fs()];
        break;
    case Qmtc2:
        if (i.word & 1u) vu0_wait(c);
        if (i.fs()) v.vf[i.fs()] = c.r[rt];
        break;
    case Cfc2: setw(c, rt, i.fs() < 16 ? v.vi[i.fs()] : cfc2(c, i.fs())); break;
    case Ctc2:
        if (i.fs() < 16) vu::set_vi(v, i.fs(), W(c, rt));
        else ctc2(c, i.fs(), W(c, rt));
        break;
#define BC v.vf[i.ft()].w[i.bc()]
    case Vaddbc: vu::addbc(v, i.dest(), i.fd(), i.fs(), BC); break;
    case Vsubbc: vu::subbc(v, i.dest(), i.fd(), i.fs(), BC); break;
    case Vmaddbc: vu::maddbc(v, i.dest(), i.fd(), i.fs(), BC); break;
    case Vmsubbc: vu::msubbc(v, i.dest(), i.fd(), i.fs(), BC); break;
    case Vmaxbc: vu::maxbc(v, i.dest(), i.fd(), i.fs(), BC); break;
    case Vminibc: vu::minibc(v, i.dest(), i.fd(), i.fs(), BC); break;
    case Vmulbc: vu::mulbc(v, i.dest(), i.fd(), i.fs(), BC); break;
    case Vmulq: vu::mulbc(v, i.dest(), i.fd(), i.fs(), v.q); break;
    case Vmaxi: vu::maxbc(v, i.dest(), i.fd(), i.fs(), v.i); break;
    case Vmuli: vu::mulbc(v, i.dest(), i.fd(), i.fs(), v.i); break;
    case Vminii: vu::minibc(v, i.dest(), i.fd(), i.fs(), v.i); break;
    case Vaddq: vu::addbc(v, i.dest(), i.fd(), i.fs(), v.q); break;
    case Vmaddq: vu::maddbc(v, i.dest(), i.fd(), i.fs(), v.q); break;
    case Vaddi: vu::addbc(v, i.dest(), i.fd(), i.fs(), v.i); break;
    case Vmaddi: vu::maddbc(v, i.dest(), i.fd(), i.fs(), v.i); break;
    case Vsubq: vu::subbc(v, i.dest(), i.fd(), i.fs(), v.q); break;
    case Vmsubq: vu::msubbc(v, i.dest(), i.fd(), i.fs(), v.q); break;
    case Vsubi: vu::subbc(v, i.dest(), i.fd(), i.fs(), v.i); break;
    case Vmsubi: vu::msubbc(v, i.dest(), i.fd(), i.fs(), v.i); break;
    case Vadd: vu::add(v, i.dest(), i.fd(), i.fs(), i.ft()); break;
    case Vmadd: vu::madd(v, i.dest(), i.fd(), i.fs(), i.ft()); break;
    case Vmul: vu::mul(v, i.dest(), i.fd(), i.fs(), i.ft()); break;
    case Vmax: vu::max(v, i.dest(), i.fd(), i.fs(), i.ft()); break;
    case Vsub: vu::sub(v, i.dest(), i.fd(), i.fs(), i.ft()); break;
    case Vmsub: vu::msub(v, i.dest(), i.fd(), i.fs(), i.ft()); break;
    case Vmini: vu::mini(v, i.dest(), i.fd(), i.fs(), i.ft()); break;
    case Vopmsub: vu::opmsub(v, i.fd(), i.fs(), i.ft()); break;
    case Viadd: vu::set_vi(v, i.fd(), v.vi[i.fs()] + v.vi[i.ft()]); break;
    case Visub: vu::set_vi(v, i.fd(), v.vi[i.fs()] - v.vi[i.ft()]); break;
    case Viaddi: vu::set_vi(v, i.ft(), v.vi[i.fs()] + i.imm5()); break;
    case Viand: vu::set_vi(v, i.fd(), v.vi[i.fs()] & v.vi[i.ft()]); break;
    case Vior: vu::set_vi(v, i.fd(), v.vi[i.fs()] | v.vi[i.ft()]); break;
    case Vcallms: vcallms(c, i.imm15() * 8u); break;
    case Vcallmsr: vcallms(c, v.cmsar0 * 8u); break;
    case Vaddabc: vu::addabc(v, i.dest(), i.fs(), BC); break;
    case Vsubabc: vu::subabc(v, i.dest(), i.fs(), BC); break;
    case Vmaddabc: vu::maddabc(v, i.dest(), i.fs(), BC); break;
    case Vmsubabc: vu::msubabc(v, i.dest(), i.fs(), BC); break;
    case Vmulabc: vu::mulabc(v, i.dest(), i.fs(), BC); break;
    case Vmulaq: vu::mulabc(v, i.dest(), i.fs(), v.q); break;
    case Vmulai: vu::mulabc(v, i.dest(), i.fs(), v.i); break;
    case Vaddaq: vu::addabc(v, i.dest(), i.fs(), v.q); break;
    case Vmaddaq: vu::maddabc(v, i.dest(), i.fs(), v.q); break;
    case Vaddai: vu::addabc(v, i.dest(), i.fs(), v.i); break;
    case Vmaddai: vu::maddabc(v, i.dest(), i.fs(), v.i); break;
    case Vsubaq: vu::subabc(v, i.dest(), i.fs(), v.q); break;
    case Vmsubaq: vu::msubabc(v, i.dest(), i.fs(), v.q); break;
    case Vsubai: vu::subabc(v, i.dest(), i.fs(), v.i); break;
    case Vmsubai: vu::msubabc(v, i.dest(), i.fs(), v.i); break;
#undef BC
    case Vadda: vu::adda(v, i.dest(), i.fs(), i.ft()); break;
    case Vmadda: vu::madda(v, i.dest(), i.fs(), i.ft()); break;
    case Vmula: vu::mula(v, i.dest(), i.fs(), i.ft()); break;
    case Vsuba: vu::suba(v, i.dest(), i.fs(), i.ft()); break;
    case Vmsuba: vu::msuba(v, i.dest(), i.fs(), i.ft()); break;
    case Vopmula: vu::opmula(v, i.fs(), i.ft()); break;
    case Vitof0: vu::itof(v, i.dest(), i.ft(), i.fs(), 0); break;
    case Vitof4: vu::itof(v, i.dest(), i.ft(), i.fs(), 4); break;
    case Vitof12: vu::itof(v, i.dest(), i.ft(), i.fs(), 12); break;
    case Vitof15: vu::itof(v, i.dest(), i.ft(), i.fs(), 15); break;
    case Vftoi0: vu::ftoi(v, i.dest(), i.ft(), i.fs(), 0); break;
    case Vftoi4: vu::ftoi(v, i.dest(), i.ft(), i.fs(), 4); break;
    case Vftoi12: vu::ftoi(v, i.dest(), i.ft(), i.fs(), 12); break;
    case Vftoi15: vu::ftoi(v, i.dest(), i.ft(), i.fs(), 15); break;
    case Vabs: vu::abs(v, i.dest(), i.ft(), i.fs()); break;
    case Vmove: vu::move(v, i.dest(), i.ft(), i.fs()); break;
    case Vmr32: vu::mr32(v, i.dest(), i.ft(), i.fs()); break;
    case Vclip: vu::clip(v, i.fs(), i.ft()); break;
    case Vnop: case Vwaitq: break;
    case Vlqi: lqc2_vu(c, i.dest(), i.ft(), v.vi[i.fs()]); vu::set_vi(v, i.fs(), v.vi[i.fs()] + 1u); break;
    case Vsqi: sqc2_vu(c, i.dest(), i.fs(), v.vi[i.ft()]); vu::set_vi(v, i.ft(), v.vi[i.ft()] + 1u); break;
    case Vlqd: vu::set_vi(v, i.fs(), v.vi[i.fs()] - 1u); lqc2_vu(c, i.dest(), i.ft(), v.vi[i.fs()]); break;
    case Vsqd: vu::set_vi(v, i.ft(), v.vi[i.ft()] - 1u); sqc2_vu(c, i.dest(), i.fs(), v.vi[i.ft()]); break;
    case Vdiv: vu::div(v, v.vf[i.fs()].w[fsf], v.vf[i.ft()].w[ftf]); break;
    case Vsqrt: vu::sqrt(v, v.vf[i.ft()].w[ftf]); break;
    case Vrsqrt: vu::rsqrt(v, v.vf[i.fs()].w[fsf], v.vf[i.ft()].w[ftf]); break;
    case Vmtir: vu::set_vi(v, i.ft(), v.vf[i.fs()].w[fsf]); break;
    case Vmfir: vu::mfir(v, i.dest(), i.ft(), i.fs()); break;
    case Vilwr: {
        std::uint16_t x;
        std::memcpy(&x, vu0_data() + vu0_addr(v.vi[i.fs()]) + 4u * std::countl_zero(i.dest() << 28), 2);
        vu::set_vi(v, i.ft(), x);
        break;
    }
    case Viswr:
        for (int k = 0; k < 4; ++k)
            if (vu::lane(i.dest(), k)) {
                const std::uint32_t x = v.vi[i.ft()];
                std::memcpy(vu0_data() + vu0_addr(v.vi[i.fs()]) + 4u * k, &x, 4);
            }
        break;
    case Vrnext:
        v.r = vu::rand_next(v.r);
        vu::apply(v, i.dest(), i.ft(), [&](int) { return v.r | fp::kOne; });
        break;
    case Vrget: vu::apply(v, i.dest(), i.ft(), [&](int) { return v.r | fp::kOne; }); break;
    case Vrinit: v.r = v.vf[i.fs()].w[fsf] & 0x7FFFFFu; break;
    case Vrxor: v.r = (v.r ^ v.vf[i.fs()].w[fsf]) & 0x7FFFFFu; break;

    default:
        stop(c, i.pc, recomp::name(i.op));
        return false;
    }
    return true;
}

}

const Insn &fetch(std::uint32_t pc) { return fetch_cached(pc); }
bool simple(Cpu &c, const Insn &i) { return simple_impl(c, i); }
bool condition(Cpu &c, const Insn &i) { return condition_impl(c, i); }

std::uint32_t g_code_epoch = 1;
std::uint64_t g_epoch_src[6] = {};
std::uint64_t g_ee_stale = 0;
const bool g_epoch_all = std::getenv("PS2_EE_EPOCH_ALL") != nullptr;
std::uint32_t g_depth = 0;

// One block (ee_core.hpp): the rules here are the JIT's contract, keep both in step.
std::uint64_t interp_block(Cpu &c) {
    std::uint32_t pc = c.pc;
    std::int32_t cost = 0;
    for (std::uint32_t n = 0;; ++n, pc += 4) {
        if (n && (n >= kMaxBlock || probe_at(pc))) {
            c.budget -= cost;
            c.pc = pc;
            return pack(kNext);
        }
        const Insn &i = fetch_cached(pc);
        const Flow fl = recomp::flow(i.op);
        if (fl == Flow::None) {
            ++cost;
            if (i.op == Syscall) {
                c.budget -= cost;
                c.pc = pc + 4;
                return pack(kSyscall, pc);
            }
            if (!simple_impl(c, i)) return pack(kHalt);
            continue;
        }
        if (i.op == Eret) {
            c.budget -= cost;
            return pack(kEret);
        }
        if (fl == Flow::Stop) {
            stop(c, pc, recomp::name(i.op));
            return pack(kHalt);
        }
        c.budget -= cost + 2;
        const Insn ins = i;  // the delay slot's fetch may evict the cached entry
        const Insn ds = fetch_cached(pc + 4);
        if (ds.op == Invalid || recomp::has_delay_slot(ds.op) || ds.op == Syscall) {
            stop(c, pc + 4, "unsupported delay slot");
            return pack(kHalt);
        }
        const std::uint32_t ret = pc + 8, t = ins.branch_target_of();
        const auto slot = [&] { return simple_impl(c, ds); };
        const auto go = [&](std::uint32_t target) {
            c.pc = target;
            return pack(target <= pc ? kBack : kNext);
        };
        switch (ins.op) {
        case Jal:
            c.r[31].sd[0] = static_cast<std::int32_t>(ret);
            if (!slot()) return pack(kHalt);
            c.pc = t;
            return pack(kCall, ret);
        case Bltzal: case Bgezal: {
            const bool taken = condition_impl(c, ins);
            c.r[31].sd[0] = static_cast<std::int32_t>(ret);
            if (!slot()) return pack(kHalt);
            c.pc = taken ? t : ret;
            return taken ? pack(kCall, ret) : pack(kNext);
        }
        case Bltzall: case Bgezall: {
            const bool taken = condition_impl(c, ins);
            c.r[31].sd[0] = static_cast<std::int32_t>(ret);
            if (taken && !slot()) return pack(kHalt);
            c.pc = taken ? t : ret;
            return taken ? pack(kCall, ret) : pack(kNext);
        }
        case Jalr: {
            const std::uint32_t target = W(c, ins.rs());
            if (ins.rd()) c.r[ins.rd()].sd[0] = static_cast<std::int32_t>(ret);
            if (!slot()) return pack(kHalt);
            c.pc = target;
            return pack(kCall, ret);
        }
        case Jr: {
            const std::uint32_t target = W(c, ins.rs());
            if (!slot()) return pack(kHalt);
            c.pc = target;
            return pack(ins.rs() == 31 ? kReturn : kJr);
        }
        case J:
            if (!slot()) return pack(kHalt);
            return go(t);
        default:
            break;
        }
        if (fl == Flow::Branch) {
            const bool taken = condition_impl(c, ins);
            if (!slot()) return pack(kHalt);
            if (taken) return go(t);
        } else if (fl == Flow::BranchLikely) {
            if (condition_impl(c, ins)) {
                if (!slot()) return pack(kHalt);
                return go(t);
            }
        } else {
            stop(c, pc, "unexpected control flow");
            return pack(kHalt);
        }
        c.pc = ret;
        return pack(kNext);
    }
}

}

namespace ps2 {

// Same contract as a generated unit function: runs until the code returns to its caller (c.pc =
// return address), a thread switch or an unwind. Calls nest through ps2::call like generated code.
void interpret(Cpu &c, std::uint32_t, std::uint32_t depth) {
    using namespace ee;
    static const bool use_jit = jit_enabled();
    g_depth = depth;
    for (;;) {
        const std::uint32_t pc = c.pc;
        if ((pc & kUnwind) || pc == kHandlerReturn || pc == kIdle) return;
        if (probe_at(pc)) [[unlikely]] {
            const bool skip = probe(c, pc);
            g_depth = depth;  // a hook may run guest code
            if (skip) {
                c.pc = c.r[31].w[0];
                return;
            }
        }
        const std::uint64_t r = use_jit ? jit_block(c) : interp_block(c);
        const std::uint32_t at = static_cast<std::uint32_t>(r >> 32);
        {  // PS2_EE_EXIT_STATS=1: exits back to this loop by kind, and the busiest kNext / kJr targets
            static const bool exit_stats = std::getenv("PS2_EE_EXIT_STATS") != nullptr;
            if (exit_stats) [[unlikely]] {
                static std::uint64_t kinds[16], total = 0;
                static std::unordered_map<std::uint32_t, std::uint64_t> next_from, jr_to;
                const unsigned k = static_cast<unsigned>(r & 0xFu);
                ++kinds[k];
                if (k == kNext) ++next_from[c.pc];
                if (k == kJr) ++jr_to[c.pc];
                if ((++total & ((1u << 22) - 1)) == 0) {
                    std::fprintf(stderr, "[ee] exits %llu: next %llu back %llu call %llu return %llu jr %llu syscall %llu other %llu\n",
                                 (unsigned long long)total, (unsigned long long)kinds[kNext], (unsigned long long)kinds[kBack],
                                 (unsigned long long)kinds[kCall], (unsigned long long)kinds[kReturn], (unsigned long long)kinds[kJr],
                                 (unsigned long long)kinds[kSyscall], (unsigned long long)(total - kinds[kNext] - kinds[kBack] - kinds[kCall] - kinds[kReturn] - kinds[kJr] - kinds[kSyscall]));
                    for (auto *m : {&next_from, &jr_to}) {
                        std::vector<std::pair<std::uint64_t, std::uint32_t>> v;
                        for (const auto &[pc, n] : *m) v.emplace_back(n, pc);
                        std::sort(v.rbegin(), v.rend());
                        std::fprintf(stderr, "[ee]   %s (%zu distinct):", m == &next_from ? "next -> pc" : "jr -> pc", v.size());
                        for (std::size_t q = 0; q < v.size() && q < 8; ++q) std::fprintf(stderr, " %08X:%llu", v[q].second, (unsigned long long)v[q].first);
                        std::fprintf(stderr, "\n");
                    }
                }
            }
        }
        switch (static_cast<Exit>(r & 0xFFu)) {
        case kNext:
        case kJr:
            continue;
        case kBack:
            if (c.budget < 0) {
                static std::uint64_t frame = 0;
                if (frame_count() != frame) {
                    frame = frame_count();
                    ++g_epoch_src[4];
                    // Safety re-check of all compiled blocks every N frames (PS2_EE_EPOCH_FRAME=N, 0 = never). It was
                    // every frame (1.1 ms per frame on the Quest: ~9500 blocks re-compared and their links broken);
                    // GT4 has no runtime-loaded code (port.sh: 0 window images) and FlushCache(2) / I-cache ops
                    // still re-check at once. [perf] "ee stale" counts blocks it ever found changed.
                    static const std::uint64_t every = [] {
                        const char *v = std::getenv("PS2_EE_EPOCH_FRAME");
                        return v ? std::strtoull(v, nullptr, 10) : 600ull;
                    }();
                    if (every && frame % every == 0) code_may_change();
                    static const bool stats = std::getenv("PS2_EE_EPOCH_STATS") != nullptr;
                    if (stats && frame % 600 == 0)
                        std::fprintf(stderr, "[ee] epoch bumps by source: icache-op %llu, dcache-op %llu, flush-icache %llu, flush-dcache %llu, frame %llu, exec %llu\n",
                                     (unsigned long long)g_epoch_src[0], (unsigned long long)g_epoch_src[1], (unsigned long long)g_epoch_src[2],
                                     (unsigned long long)g_epoch_src[3], (unsigned long long)g_epoch_src[4], (unsigned long long)g_epoch_src[5]);
                }
                if (preempt(c)) {
                    c.pc |= kUnwind;
                    return;
                }
            }
            continue;
        case kCall:
            call(c, c.pc, depth + 1u);
            g_depth = depth;
            if (c.pc != at) [[unlikely]] return;
            continue;
        case kReturn:
            return;
        case kSyscall: {
            std::uint32_t n = c.r[3].w[0];
            if (n & 0x80000000u) n = 0u - n;
            // FlushCache / iFlushCache(op): 0 = write back D-cache, 1 = invalidate D-cache (data only), 2 = invalidate
            // I-cache, 3 = both (ps2sdk kernel.h). LoadExecPS2 / ExecPS2 always.
            if (n == 0x64 || n == 0x68) {
                const std::uint32_t op = c.r[4].w[0];
                const bool icache = op > 1u;
                ++g_epoch_src[icache ? 2 : 3];
                if (icache || g_epoch_all) code_may_change();
            } else if (n == 0x06 || n == 0x07) {
                ++g_epoch_src[5];
                code_may_change();
            }
            syscall(c, at);
            g_depth = depth;
            if (c.pc != at + 4) return;
            continue;
        }
        case kEret:
            c.pc = eret(c);
            continue;
        case kHalt:
            return;
        }
    }
}

#if !defined(__aarch64__)
namespace ee {
bool jit_enabled() { return false; }
std::uint64_t jit_block(Cpu &c) { return interp_block(c); }
void jit_flush() {}
}
#endif

}

namespace ps2 {
std::uint64_t ee_stale_blocks() { return ee::g_ee_stale; }
std::uint64_t ee_epoch_bumps() { return ee::g_code_epoch; }
}  // namespace ps2

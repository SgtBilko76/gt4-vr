#pragma once
// EE execution core shared by the interpreter (ee_interp.cpp) and the ARM64 JIT (ee_jit_a64.cpp).
//
// Both run the game in BLOCKS: from c.pc through straight-line instructions up to and including the
// first control-flow instruction and its delay slot (or a syscall/eret, or kMaxBlock instructions,
// or the instruction before a probe address). A block returns an Exit packed with a pc; the frame
// loop in ee_interp.cpp turns it into the generated code's protocol (nested calls, returns,
// preemption on backward jumps). Interpreter and JIT use the same block boundaries and charge the
// budget at the same points, so their runs are bit-identical (the JIT's correctness test).
#include "ps2/generated.hpp"

#include "../../recomp/r5900.hpp"

#include <cstdint>

namespace ps2 {
bool probe_at(std::uint32_t pc);
}

namespace ps2::ee {

enum Exit : std::uint32_t {
    kNext,     // c.pc = next block (fallthrough, forward jump, not-taken branch)
    kBack,     // c.pc = target of a taken jump/branch to itself or backwards: preemption point
    kCall,     // c.pc = callee; packed pc = return address
    kReturn,   // jr $ra: c.pc = return address, leave this frame
    kJr,       // other jr: c.pc = target, stay in this frame
    kSyscall,  // c.pc = pc + 4; packed pc = the syscall's address
    kEret,     // eret: frame loop sets c.pc = eret(c)
    kHalt,     // stopped (c.pc has kUnwind)
};

inline constexpr std::uint64_t pack(Exit e, std::uint32_t pc = 0) { return e | (static_cast<std::uint64_t>(pc) << 32); }

inline constexpr std::uint32_t kMaxBlock = 128;

// Budget charged per instruction: 1, branch + delay slot together 2 (as codegen.cpp). A block
// charges what it ran so far right before its control instruction, or at its end.

const recomp::Insn &fetch(std::uint32_t pc);
bool simple(Cpu &c, const recomp::Insn &i);   // one non-control instruction; false = stopped
bool condition(Cpu &c, const recomp::Insn &i); // branch condition
std::uint64_t interp_block(Cpu &c);

// Code epoch: a translated block checks its guest words once per epoch. The epoch moves wherever
// game code can have changed, as on the real EE with its instruction cache: syscalls (FlushCache,
// FlushCache / iFlushCache / ExecPS2 syscalls (a game must flush after writing or loading code),
// the CACHE instruction, and every new frame.
extern std::uint32_t g_code_epoch;
inline void code_may_change() { ++g_code_epoch; }

// Guest call depth of the innermost running interpret() frame: a translated block calls a function
// itself (ee_h_call -> ps2::call) and needs the depth the generated code would pass.
extern std::uint32_t g_depth;

// JIT (ARM64 only). jit_enabled(): built for ARM64 and PS2_EE_JIT is not 0.
bool jit_enabled();
std::uint64_t jit_block(Cpu &c);
void jit_flush();

}

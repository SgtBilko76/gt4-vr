// EE JIT check (ARM64; run under qemu-aarch64 on the PC): every natively translated instruction,
// with random registers and immediates, as a one-block program "insn; delay/branch; ..." compared
// against the interpreter (ee::interp_block). Prints the first mismatches per opcode.
//   build: see quest/tools or the ee-interpreter notes; run: qemu-aarch64 ee_jit_test [iterations]
#include "../runtime/src/ee_core.hpp"
#include "ps2/memory.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

void register_generated_code() {}

namespace {

using namespace ps2;

std::mt19937_64 rng64(1234);
std::uint32_t rng() { return static_cast<std::uint32_t>(rng64()); }

std::uint32_t enc_r(unsigned op, unsigned rs, unsigned rt, unsigned rd, unsigned sa, unsigned fn) {
    return op << 26 | rs << 21 | rt << 16 | rd << 11 | sa << 6 | fn;
}
std::uint32_t enc_i(unsigned op, unsigned rs, unsigned rt, std::uint32_t imm) { return op << 26 | rs << 21 | rt << 16 | (imm & 0xFFFFu); }

unsigned reg() {
    const unsigned r = rng() % 34;
    return r >= 32 ? 0 : r;  // $zero more often
}
std::uint64_t value() {
    switch (rng() % 6) {
    case 0: return 0;
    case 1: return rng() & 0xFF;
    case 2: return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(rng())));
    case 3: return 0x00100000u + (rng() % 0x100000u);  // RAM addresses
    case 4: return ~0ull - (rng() & 0xFF);
    default: return rng64();
    }
}

struct Case {
    const char *name;
    std::uint32_t (*make)();
};

constexpr std::uint32_t kBase = 0x00200000u;  // test code
constexpr std::uint32_t kData = 0x00100000u;  // loads/stores hit around here

std::uint32_t imm16() {
    switch (rng() % 4) {
    case 0: return rng() & 0xF;
    case 1: return 0x10000u - (rng() & 0xF);
    case 2: return rng() & 0xFFF;
    default: return rng() & 0xFFFF;
    }
}

#define R(NAME, FN) {NAME, [] { return enc_r(0, reg(), reg(), reg(), static_cast<unsigned>(rng() % 32), FN); }}
#define I(NAME, OP) {NAME, [] { return enc_i(OP, reg(), reg(), imm16()); }}
std::vector<Case> cases = {
    R("sll", 0x00), R("srl", 0x02), R("sra", 0x03), R("sllv", 0x04), R("srlv", 0x06), R("srav", 0x07),
    R("movz", 0x0A), R("movn", 0x0B), R("mfhi", 0x10), R("mthi", 0x11), R("mflo", 0x12), R("mtlo", 0x13),
    R("dsllv", 0x14), R("dsrlv", 0x16), R("dsrav", 0x17),
    R("add", 0x20), R("addu", 0x21), R("sub", 0x22), R("subu", 0x23), R("and", 0x24), R("or", 0x25), R("xor", 0x26),
    R("nor", 0x27), R("slt", 0x2A), R("sltu", 0x2B), R("dadd", 0x2C), R("daddu", 0x2D), R("dsub", 0x2E), R("dsubu", 0x2F),
    R("dsll", 0x38), R("dsrl", 0x3A), R("dsra", 0x3B), R("dsll32", 0x3C), R("dsrl32", 0x3E), R("dsra32", 0x3F),
    I("addi", 0x08), I("addiu", 0x09), I("slti", 0x0A), I("sltiu", 0x0B), I("andi", 0x0C), I("ori", 0x0D), I("xori", 0x0E),
    I("lui", 0x0F), I("daddi", 0x18), I("daddiu", 0x19),
    {"mfc1", [] { return 0x44000000u | reg() << 16 | (rng() % 32) << 11; }},
    {"mtc1", [] { return 0x44800000u | reg() << 16 | (rng() % 32) << 11; }},
    {"mov.s", [] { return 0x46000006u | (rng() % 32) << 11 | (rng() % 32) << 6; }},
    R("mult", 0x18), R("multu", 0x19),
#define F(NAME, FN) {NAME, [] { return 0x46000000u | (rng() % 32) << 16 | (rng() % 32) << 11 | (rng() % 32) << 6 | FN; }}
    F("add.s", 0x00), F("sub.s", 0x01), F("mul.s", 0x02), F("sqrt.s", 0x04), F("abs.s", 0x05), F("neg.s", 0x07),
    F("adda.s", 0x18), F("suba.s", 0x19), F("mula.s", 0x1A), F("madd.s", 0x1C), F("msub.s", 0x1D), F("madda.s", 0x1E),
    F("msuba.s", 0x1F), F("cvt.w.s", 0x24), F("max.s", 0x28), F("min.s", 0x29), F("c.eq.s", 0x32), F("c.lt.s", 0x34),
    F("c.le.s", 0x36),
    {"cvt.s.w", [] { return 0x46800020u | (rng() % 32) << 11 | (rng() % 32) << 6; }},
};
// memory: base register forced to a RAM address, offsets small
#define M(NAME, OP) {NAME, [] { return enc_i(OP, 1 + rng() % 31, reg(), imm16() & 0x8FFFu); }}
std::vector<Case> mem_cases = {
    M("lb", 0x20), M("lh", 0x21), M("lw", 0x23), M("lbu", 0x24), M("lhu", 0x25), M("lwu", 0x27), M("ld", 0x37),
    M("sb", 0x28), M("sh", 0x29), M("sw", 0x2B), M("sd", 0x3F), M("lq", 0x1E), M("sq", 0x1F),
    {"lwc1", [] { return enc_i(0x31, 1 + rng() % 31, rng() % 32, imm16() & 0x8FFFu); }},
    {"swc1", [] { return enc_i(0x39, 1 + rng() % 31, rng() % 32, imm16() & 0x8FFFu); }},
};
// branches: target forward inside the test page
std::vector<Case> branch_cases = {
    {"beq", [] { return enc_i(0x04, reg(), reg(), 8); }}, {"bne", [] { return enc_i(0x05, reg(), reg(), 8); }},
    {"blez", [] { return enc_i(0x06, reg(), 0, 8); }}, {"bgtz", [] { return enc_i(0x07, reg(), 0, 8); }},
    {"beql", [] { return enc_i(0x14, reg(), reg(), 8); }}, {"bnel", [] { return enc_i(0x15, reg(), reg(), 8); }},
    {"blezl", [] { return enc_i(0x16, reg(), 0, 8); }}, {"bgtzl", [] { return enc_i(0x17, reg(), 0, 8); }},
    {"bltz", [] { return enc_i(0x01, reg(), 0x00, 8); }}, {"bgez", [] { return enc_i(0x01, reg(), 0x01, 8); }},
    {"bltzl", [] { return enc_i(0x01, reg(), 0x02, 8); }}, {"bgezl", [] { return enc_i(0x01, reg(), 0x03, 8); }},
    {"bltzal", [] { return enc_i(0x01, reg(), 0x10, 8); }}, {"bgezal", [] { return enc_i(0x01, reg(), 0x11, 8); }},
    {"bc1f", [] { return 0x45000008u; }}, {"bc1t", [] { return 0x45010008u; }},
    {"bc1fl", [] { return 0x45020008u; }}, {"bc1tl", [] { return 0x45030008u; }},
    {"j", [] { return 0x08000000u | ((kBase + 0x40) >> 2); }}, {"jal", [] { return 0x0C000000u | ((kBase + 0x40) >> 2); }},
    {"jr", [] { return enc_r(0, reg(), 0, 0, 0, 0x08); }}, {"jalr", [] { return enc_r(0, 1 + rng() % 31, 0, reg(), 0, 0x09); }},
};

void randomize(Cpu &c, bool ram_bases) {
    std::memset(&c, 0, sizeof c);
    for (int r = 1; r < 32; ++r) {
        c.r[r].d[0] = ram_bases ? kData + (rng() % 0x8000u) * 4u : value();
        c.r[r].d[1] = rng64();
    }
    c.hi.d[0] = rng64(), c.lo.d[0] = rng64(), c.hi.d[1] = rng64(), c.lo.d[1] = rng64();
    for (auto &f : c.f) f = static_cast<std::uint32_t>(rng());
    c.facc = static_cast<std::uint32_t>(rng());
    c.fcr31 = static_cast<std::uint32_t>(rng()) & (1u << 23);
    c.budget = 1000;
    c.pc = kBase;
}

int failures = 0;

bool same(const Cpu &a, const Cpu &b, std::string &why) {
    for (int r = 0; r < 32; ++r)
        if (std::memcmp(&a.r[r], &b.r[r], 16)) { why = "r" + std::to_string(r); return false; }
    if (std::memcmp(&a.hi, &b.hi, 16) || std::memcmp(&a.lo, &b.lo, 16)) { why = "hi/lo"; return false; }
    if (std::memcmp(a.f, b.f, sizeof a.f)) { why = "f"; return false; }
    if (a.pc != b.pc) { why = "pc"; return false; }
    if (a.facc != b.facc || a.fcr31 != b.fcr31) { why = "facc/fcr31"; return false; }
    if (a.budget != b.budget) { why = "budget"; return false; }
    return true;
}

void run(const std::vector<Case> &list, bool ram_bases, bool as_branch, int iterations) {
    static std::vector<std::uint8_t> before(0x40000), after_i(0x40000);
    for (const Case &k : list) {
        int bad = 0;
        for (int n = 0; n < iterations; ++n) {
            const std::uint32_t word = k.make();
            // block: [insn] [delay: addiu r9, r9, 1 / or a 2nd random insn] ... nops ... at +0x40: jr ra
            const std::uint32_t second = as_branch ? enc_i(0x09, 9, 9, 1) : 0x00000000u;
            std::memset(g_ram + kBase, 0, 0x100);
            std::memcpy(g_ram + kBase, &word, 4);
            std::memcpy(g_ram + kBase + 4, &second, 4);
            const std::uint32_t jr = 0x03E00008u;  // jr ra (ends a non-branch block: kMaxBlock is far away)
            std::memcpy(g_ram + kBase + 8, &jr, 4);
            // callees (branch target +0x24, jal target +0x40) return at once
            std::memcpy(g_ram + kBase + 0x24, &jr, 4);
            std::memcpy(g_ram + kBase + 0x40, &jr, 4);
            Cpu ci;
            randomize(ci, ram_bases);
            if ((word >> 26) == 0 && (word & 0x3F) == 0x09) ci.r[(word >> 21) & 31].d[0] = kBase + 0x40;  // jalr
            Cpu cj = ci;
            std::memcpy(before.data(), g_ram + kData, before.size());
            ee::jit_flush();
            std::uint64_t ri = ee::interp_block(ci);
            // the interpreter leaves calls to the frame loop, translated code makes them in place
            if ((ri & 0xFF) == ee::kCall) {
                const std::uint32_t ret = static_cast<std::uint32_t>(ri >> 32);
                call(ci, ci.pc, 1);
                ri = ci.pc == ret ? ee::pack(ee::kNext) : ee::pack(ee::kReturn);
            }
            std::memcpy(after_i.data(), g_ram + kData, after_i.size());
            std::memcpy(g_ram + kData, before.data(), before.size());
            ee::g_depth = 0;
            const std::uint64_t rj = ee::jit_block(cj);
            std::string why;
            const bool mem_ok = std::memcmp(after_i.data(), g_ram + kData, after_i.size()) == 0;
            if (ri != rj) why = "exit";
            else if (!mem_ok) why = "memory";
            if (!mem_ok && bad < 3)
                for (std::size_t b = 0; b < after_i.size(); ++b)
                    if (after_i[b] != g_ram[kData + b]) {
                        std::printf("  first diff at %08zX: interp %02X jit %02X (before %02X)\n", kData + b, after_i[b], g_ram[kData + b], before[b]);
                        break;
                    }
            if ((!why.empty() || !same(ci, cj, why)) && bad++ < 3) {
                std::printf("FAIL %-7s word %08X: %s (interp exit %llx, jit %llx)\n", k.name, word, why.c_str(),
                            static_cast<unsigned long long>(ri), static_cast<unsigned long long>(rj));
                ++failures;
            }
            std::memcpy(g_ram + kData, before.data(), before.size());
        }
        if (!bad) std::printf("ok   %s\n", k.name);
    }
}

}

int main(int argc, char **argv) {
    const int iterations = argc > 1 ? std::atoi(argv[1]) : 2000;
    // random data, so a load from a wrong address shows (zeros everywhere would hide it)
    for (std::uint32_t a = 0x000F0000u; a < 0x00140000u; ++a) ps2::g_ram[a] = static_cast<std::uint8_t>(rng());
    setenv("PS2_EE_JIT", "1", 1);
    if (!ps2::ee::jit_enabled()) {
        std::printf("JIT not available on this build\n");
        return 1;
    }
    run(cases, false, false, iterations);
    run(mem_cases, true, false, iterations);
    run(branch_cases, false, true, iterations);
    std::printf("%s: %d failures\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}

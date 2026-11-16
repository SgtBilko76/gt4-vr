// PS2_RR5_LZCHECK=1 (PC ps2run only, diagnosis): every call of Ridge Racer V's LZSS unpacker func_221D68 -
// fresh AND resumed ones (input arriving in pieces) - is unpacked again on the host with the Fukami port's
// byte loop (fukami-wt-runtime src/product/patches.cpp rrvUnpack, slow path) from the guest state at entry,
// and compared at the return address: output bytes, return value and the gp state words.
#include "ps2/system.hpp"
#include "ps2/cpu.hpp"
#include "ps2/memory.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace {

struct Expect {
    bool ok = false;
    std::uint32_t v0 = 0, base = 0, gp = 0, ra = 0, sp = 0, a0 = 0, a2 = 0, resumed = 0;
    std::vector<std::uint8_t> out;
    std::map<std::uint32_t, std::uint32_t> words;
};

std::vector<Expect> g_stack;
std::uint64_t g_calls = 0, g_bad = 0, g_resumed = 0;

std::uint8_t ram8(std::uint32_t a) { return ps2::g_ram[a & (ps2::kRamSize - 1)]; }
std::uint32_t ram32(std::uint32_t a) { std::uint32_t v; std::memcpy(&v, ps2::g_ram + (a & (ps2::kRamSize - 1)), 4); return v; }

void unpack(const ps2::Cpu &c, Expect &r) {
    const std::uint32_t gp = c.r[28].w[0];
    auto &w = r.words;
    auto get = [&](std::uint32_t off) { auto it = w.find(off); return it != w.end() ? it->second : ram32(gp - off); };
    auto put = [&](std::uint32_t off, std::uint32_t v) { w[off] = v; };
    auto rd8 = [&](std::uint32_t a) -> std::uint32_t { return a - r.base < r.out.size() ? r.out[a - r.base] : ram8(a); };
    auto reset = [&] { put(0x6B04, 0); put(0x6B00, 0); };
    std::uint32_t s0 = c.r[4].w[0], s1 = c.r[5].w[0];
    const std::uint32_t s5 = c.r[6].w[0], a3in = c.r[7].w[0], t0 = c.r[8].w[0];
    r.base = s1;
    r.ok = true;
    if (s0 == s5 && s5 != 0) { r.v0 = s1; return; }
    if (get(0x5B70) == s5 && s5 != 0) { r.v0 = s1; return; }
    const std::uint32_t t5 = get(0x6B04);
    r.resumed = t5;
    std::uint32_t s6 = get(0x5B6C), s7, t4, s3, s4, fp;
    if (t5 == 0) {
        s6 = rd8(s0++);
        s7 = (1u << (s6 & 31u)) - 1u;
        t4 = 1u << ((16u - s6) & 31u);
        put(0x5B6C, s6); put(0x5B68, s7); put(0x5B64, t4);
        s3 = rd8(s0++); put(0x5B60, s3);
        std::uint32_t a2 = 0, a1 = 0;
        for (std::uint32_t i = 0; i < 4; ++i) a2 += rd8(s0++) << (i * 8u);
        put(0x5B5C, a2);
        for (std::uint32_t i = 0; i < 4; ++i) a1 += rd8(s0++) << (i * 8u);
        fp = s1 + a2; s4 = s0 + a1;
        put(0x5B58, a1); put(0x5B54, s4); put(0x5B50, fp);
        if (!(s6 - 1u < 14u) || s3 != 8u || a2 != a3in || a1 != t0 - 10u) { put(0x6B00, 1); r.v0 = s1; return; }
        if (get(0x6B00) != 0) put(0x6B00, 0xFFFFFFFFu);
    } else {
        s7 = get(0x5B68); t4 = get(0x5B64); s3 = get(0x5B60); s4 = get(0x5B54); fp = get(0x5B50);
        s0 = get(0x5B78); s1 = get(0x5B74);
        r.base = s1;
    }
    put(0x6B04, t5 + 1u);
    const auto shortInput = [&] { return s5 != 0 && s5 - (s3 * 16u + 1u) < s0; };
    if (shortInput()) { put(0x5B78, s0); put(0x5B70, s5); put(0x5B74, s1); r.v0 = s1; return; }
    const std::uint32_t a3 = s7 + 1u;
    for (;;) {
        std::uint64_t s2 = 0;
        for (std::uint32_t t1 = 0; t1 < s3; ++t1) {
            s2 += std::uint64_t(rd8(s0++)) << ((t1 * 8u) & 63u);
            if (!(s0 < s4)) { reset(); break; }
        }
        for (std::int32_t t1 = std::int32_t(s3 * 8u); t1 > 0; --t1) {
            const bool literal = (s2 & 1u) != 0; s2 >>= 1;
            if (literal) {
                r.out.push_back(std::uint8_t(rd8(s0++))); ++s1;
            } else {
                const std::uint32_t hi = rd8(s0++), lo = rd8(s0++), v1 = lo | (hi << 8);
                std::uint32_t off = v1 & s7, len = std::uint32_t(std::int32_t(v1) >> (s6 & 31u));
                if (off == 0) off = a3;
                if (len == 0) len = t4;
                std::uint32_t a1 = s1 - off;
                for (std::uint32_t i = 0; i < len; ++i) { r.out.push_back(std::uint8_t(rd8(a1++))); ++s1; }
            }
            if (r.out.size() > 0x2000000u) { r.ok = false; return; }
            if (!(s0 < s4) || !(s1 < fp)) { reset(); r.v0 = s1; return; }
        }
        if (shortInput()) { put(0x5B78, s0); put(0x5B70, s5); put(0x5B74, s1); r.v0 = s1; return; }
    }
}

void compare(ps2::Cpu &c) {
    if (g_stack.empty()) return;
    const std::uint32_t pc = c.pc;
    // Only the matching frame (same return address and stack pointer).
    for (std::size_t k = g_stack.size(); k-- > 0;) {
        Expect &e = g_stack[k];
        if (e.ra != pc || e.sp != c.r[29].w[0]) continue;
        ++g_calls;
        if (e.resumed) ++g_resumed;
        std::size_t diff = SIZE_MAX;
        for (std::size_t i = 0; i < e.out.size(); ++i)
            if (ram8(e.base + std::uint32_t(i)) != e.out[i]) { diff = i; break; }
        std::uint32_t bad_word = 0;
        for (const auto &[off, v] : e.words)
            if (ram32(e.gp - off) != v) { bad_word = off; break; }
        const bool v0_bad = c.r[2].w[0] != e.v0;
        if (!e.ok || diff != SIZE_MAX || bad_word || v0_bad) {
            ++g_bad;
            std::fprintf(stderr, "[lzcheck] call %llu (resume %u, in 0x%08X avail 0x%08X, out 0x%08X+0x%zX): ok=%d",
                         static_cast<unsigned long long>(g_calls), e.resumed, e.a0, e.a2, e.base, e.out.size(), e.ok);
            if (diff != SIZE_MAX)
                std::fprintf(stderr, " first diff at +0x%zX (0x%08X): guest %02X host %02X", diff, e.base + std::uint32_t(diff),
                             ram8(e.base + std::uint32_t(diff)), e.out[diff]);
            if (bad_word) std::fprintf(stderr, " state gp-0x%X guest %08X host %08X", bad_word, ram32(e.gp - bad_word), e.words[bad_word]);
            if (v0_bad) std::fprintf(stderr, " v0 guest %08X host %08X", c.r[2].w[0], e.v0);
            std::fprintf(stderr, "\n");
        }
        if ((g_calls % 50) == 0 || g_calls < 5)
            std::fprintf(stderr, "[lzcheck] %llu calls (%llu resumed), %llu differ\n", static_cast<unsigned long long>(g_calls),
                         static_cast<unsigned long long>(g_resumed), static_cast<unsigned long long>(g_bad));
        g_stack.erase(g_stack.begin() + static_cast<std::ptrdiff_t>(k));
        return;
    }
}

} // namespace

void rr5_install_lzcheck() {
    ps2::set_probe_hook(0x00221D68u, [](ps2::Cpu &c) {
        Expect e;
        e.gp = c.r[28].w[0];
        e.ra = c.r[31].w[0];
        e.sp = c.r[29].w[0];
        e.a0 = c.r[4].w[0];
        e.a2 = c.r[6].w[0];
        unpack(c, e);
        if (e.ra != 0x00222338u) std::fprintf(stderr, "[lzcheck] unexpected caller ra 0x%08X\n", e.ra);
        g_stack.push_back(std::move(e));
    });
    // The only call site is the jal at 0x222330 in func_222258.
    ps2::set_probe_hook(0x00222338u, compare);
    std::fprintf(stderr, "[lzcheck] checking every call of func_221D68 (fresh and resumed)\n");
}

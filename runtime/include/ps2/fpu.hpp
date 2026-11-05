#pragma once

#include <bit>
#include <cmath>
#include <cstdint>
#include <utility>

namespace ps2::fp {

inline constexpr std::uint32_t kSign = 0x80000000u;
inline constexpr std::uint32_t kAbs = 0x7FFFFFFFu;
inline constexpr std::uint32_t kOne = 0x3F800000u;

inline std::uint32_t exponent(std::uint32_t x) { return (x >> 23) & 0xFFu; }
inline std::uint32_t flush(std::uint32_t x) { return exponent(x) ? x : (x & kSign); }
inline std::uint32_t mantissa(std::uint32_t x) { return (x & 0x7FFFFFu) | 0x800000u; }

inline std::uint32_t pack(std::uint32_t sign, int e, std::uint32_t m) {
    if (e > 255) return sign | kAbs;
    if (e <= 0) return sign;
    return sign | (static_cast<std::uint32_t>(e) << 23) | (m & 0x7FFFFFu);
}

inline std::uint32_t add(std::uint32_t a, std::uint32_t b) {
    a = flush(a);
    b = flush(b);
    std::uint32_t aa = a & kAbs, ba = b & kAbs;
    if (!ba) return aa ? a : (a & b);
    if (!aa) return b;
    if (ba > aa) {
        std::swap(a, b);
        std::swap(aa, ba);
    }
    int e = static_cast<int>(aa >> 23);
    const int diff = e - static_cast<int>(ba >> 23);
    std::uint32_t ma = mantissa(aa) << 1;
    const std::uint32_t mb = diff > 25 ? 0u : (mantissa(ba) << 1) >> diff;
    if ((a ^ b) & kSign) {
        ma -= mb;
        if (!ma) return 0;
        const int shift = std::countl_zero(ma) - 7;
        ma <<= shift;
        e -= shift;
    } else {
        ma += mb;
        if (ma & 0x2000000u) {
            ma >>= 1;
            ++e;
        }
    }
    return pack(a & kSign, e, ma >> 1);
}

inline std::uint32_t sub(std::uint32_t a, std::uint32_t b) { return add(a, b ^ kSign); }

inline std::uint32_t mul(std::uint32_t a, std::uint32_t b) {
    const std::uint32_t sign = (a ^ b) & kSign;
    const int ea = static_cast<int>(exponent(a)), eb = static_cast<int>(exponent(b));
    if (!ea || !eb) return sign;
    std::uint64_t m = static_cast<std::uint64_t>(mantissa(a)) * mantissa(b);
    int e = ea + eb - 127;
    if (m >> 47) {
        m >>= 24;
        ++e;
    } else {
        m >>= 23;
    }
    return pack(sign, e, static_cast<std::uint32_t>(m));
}

inline double to_double(std::uint32_t x) {
    if (!exponent(x)) return (x & kSign) ? -0.0 : 0.0;
    const std::uint64_t bits = (static_cast<std::uint64_t>(x & kSign) << 32) |
                               (static_cast<std::uint64_t>(exponent(x) + 1023 - 127) << 52) |
                               (static_cast<std::uint64_t>(x & 0x7FFFFFu) << 29);
    return std::bit_cast<double>(bits);
}

inline std::uint32_t from_double(double d) {
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(d);
    const std::uint32_t sign = static_cast<std::uint32_t>(bits >> 32) & kSign;
    const int e = static_cast<int>((bits >> 52) & 0x7FFu);
    if (!e) return sign;
    return pack(sign, e - 1023 + 127, static_cast<std::uint32_t>(bits >> 29));
}

// Division rounds to nearest (ties to even), the other FPU ops chop. PCSX2's default EE settings
// (eeRoundMode chop, eeDivRoundMode nearest) - GT4 depends on it: its file decryption (0x579E00)
// runs a float key stream through mul.s/div.s, and a chopped div.s turns the license-test data
// into garbage (runaway copy -> crash when a license test starts). The double quotient of two
// singles rounds correctly to single (53 >= 2*24+2 bits), so no double-rounding error.
inline std::uint32_t from_double_nearest(double d) {
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(d);
    const std::uint32_t sign = static_cast<std::uint32_t>(bits >> 32) & kSign;
    int e = static_cast<int>((bits >> 52) & 0x7FFu);
    if (!e) return sign;
    std::uint64_t m = (bits & 0xFFFFFFFFFFFFFull) | (1ull << 52);  // 53 bits
    const std::uint64_t rest = m & ((1ull << 29) - 1), half = 1ull << 28;
    m >>= 29;                                                         // 24 bits
    if (rest > half || (rest == half && (m & 1))) {
        ++m;
        if (m >> 24) {
            m >>= 1;
            ++e;
        }
    }
    return pack(sign, e - 1023 + 127, static_cast<std::uint32_t>(m));
}

struct Result {
    std::uint32_t value;
    bool invalid;
};

// ---- EE FPU (COP1) multiply / divide as the PS2 hardware computes them.
// Ported from PCSX2 soft-float (PR #12001, GPL-3.0+, GitHubProUser67 et al.: Booth multiplier
// after Gregory Gaines' research; radix divider after DOI 10.1109/ARITH.1995.465363).
// GT4 needs it bit-exact: its file decryption (0x579E00) runs a float key stream through mul.s /
// div.s and saturates after ~250 bytes, so the output depends on every bit. Verified on all 125
// license files of the retail disc: hardware mul+div = 125/125 correct (game's own CRC), the IEEE
// versions 0/125 (license tests crashed while loading, owner 2026-11-05). The VU units keep the
// plain versions (PCSX2 found accurate VU mul/div harmful there).
namespace hw {
struct CarrySave {
    std::uint32_t sum, carry;
};
inline CarrySave csa(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    const std::uint32_t u = a ^ b;
    return {u ^ c, ((a & b) | (u & c)) << 1};
}
inline std::int32_t quotient_select(CarrySave cur) {
    const std::uint32_t mask = (1u << 24) - 1;  // the point sits between bits 24 and 25
    const auto test = static_cast<std::int32_t>(((cur.sum & ~mask) + cur.carry) | (cur.sum & mask));
    if (test >= (1 << 23)) return 1;
    if (test < static_cast<std::int32_t>(~0u << 24)) return -1;
    return 0;
}
struct Booth {
    std::uint32_t data, negate;
};
inline Booth booth(std::uint32_t a, std::uint32_t b, std::uint32_t bit) {
    const std::uint32_t test = (bit ? b >> (bit * 2 - 1) : b << 1) & 7;
    a <<= bit * 2;
    a += (test == 3 || test == 4) ? a : 0;
    const std::uint32_t neg = (test >= 4 && test <= 6) ? ~0u : 0;
    const std::uint32_t pos = 1u << (bit * 2);
    a ^= neg & (0u - pos);
    a &= (test >= 1 && test <= 6) ? ~0u : 0;
    return {a, neg & pos};
}
inline CarrySave add3(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    const std::uint32_t u = a ^ b;
    return {u ^ c, ((u & c) | (a & b)) << 1};
}
inline std::uint64_t mul_mantissa(std::uint32_t a, std::uint32_t b) {
    const std::uint64_t full = static_cast<std::uint64_t>(a) * b;
    const Booth b0 = booth(a, b, 0), b1 = booth(a, b, 1), b2 = booth(a, b, 2), b3 = booth(a, b, 3);
    const Booth b4 = booth(a, b, 4), b5 = booth(a, b, 5), b6 = booth(a, b, 6);
    Booth b7 = booth(a, b, 7);
    const CarrySave t0 = add3(b1.data, b2.data, b3.data);
    CarrySave t1 = add3(b4.data & ~0x7FFu, b5.data & ~0xFFFu, b6.data);
    t1.carry |= b6.negate | (b5.data & 0x800);
    b7.data |= (b5.data & 0x400) + b5.negate;
    const CarrySave t2 = add3(b0.data, t0.sum, t0.carry);
    const CarrySave t3 = add3(b7.data, t1.sum, t1.carry);
    const CarrySave t4 = add3(t2.carry, t3.sum, t3.carry);
    CarrySave t5 = add3(t2.sum, t4.sum, t4.carry);
    t5.carry += b7.negate;
    t5.sum &= ~0x7FFFu;
    t5.carry &= ~0x7FFFu;
    const std::uint32_t lo = t5.sum + t5.carry;
    return full - ((lo ^ full) & 0x8000);
}
}  // namespace hw

inline std::uint32_t fpu_mul(std::uint32_t a, std::uint32_t b) {
    const std::uint32_t sign = (a ^ b) & kSign;
    if (!exponent(a) || !exponent(b)) return sign;
    int e = static_cast<int>(exponent(a)) + static_cast<int>(exponent(b)) - 127;
    auto m = static_cast<std::uint32_t>(hw::mul_mantissa(mantissa(a), mantissa(b)) >> 23);
    if (m > 0xFFFFFFu) {
        m >>= 1;
        ++e;
    }
    if (e > 255) return sign | kAbs;
    if (e < 1) return sign;
    return sign | (static_cast<std::uint32_t>(e) << 23) | (m & 0x7FFFFFu);
}

inline Result fpu_div(std::uint32_t a, std::uint32_t b) {
    const std::uint32_t sign = (a ^ b) & kSign;
    const bool a_zero = !exponent(a), b_zero = !exponent(b);
    if (b_zero) return {sign | kAbs, true};  // x/0 and 0/0: max with the sign, flags set
    if (a_zero) return {sign, false};
    std::uint32_t am = mantissa(a) << 2, bm = mantissa(b) << 2;
    hw::CarrySave cur{am, 0};
    std::uint32_t q = 0;
    int qbit = 1;
    for (int i = 0; i < 25; ++i) {
        q = (q << 1) + static_cast<std::uint32_t>(qbit);
        const std::uint32_t add = qbit > 0 ? ~bm : qbit < 0 ? bm : 0;
        cur.carry += qbit > 0;
        const hw::CarrySave s = hw::csa(cur.sum, cur.carry, add);
        qbit = hw::quotient_select(qbit ? s : cur);
        cur.sum = s.sum << 1;
        cur.carry = s.carry << 1;
    }
    int e = static_cast<int>(exponent(a)) - static_cast<int>(exponent(b)) + 126;
    if (q >= (1u << 24)) {
        ++e;
        q >>= 1;
    }
    if (e > 255) return {sign | kAbs, false};
    if (e < 1) return {sign, false};
    return {sign | (static_cast<std::uint32_t>(e) << 23) | (q & 0x7FFFFFu), false};
}

inline Result div(std::uint32_t a, std::uint32_t b) {
    a = flush(a);
    b = flush(b);
    const std::uint32_t sign = (a ^ b) & kSign;
    if (!(b & kAbs)) return {sign | kAbs, true};
    if (!(a & kAbs)) return {sign, false};
    return {from_double_nearest(to_double(a) / to_double(b)), false};
}

inline std::uint32_t sqrt(std::uint32_t a) {
    a = flush(a) & kAbs;
    if (!a) return 0;
    return from_double(std::sqrt(to_double(a)));
}

inline Result rsqrt(std::uint32_t a, std::uint32_t b) {
    a = flush(a);
    b = flush(b) & kAbs;
    if (!b) return {(a & kSign) | kAbs, true};
    if (!(a & kAbs)) return {a & kSign, false};
    return {from_double(to_double(a) / std::sqrt(to_double(b))), false};
}

inline std::int32_t key(std::uint32_t x) {
    x = flush(x);
    const auto magnitude = static_cast<std::int32_t>(x & kAbs);
    return (x & kSign) ? -magnitude : magnitude;
}

inline std::uint32_t max(std::uint32_t a, std::uint32_t b) { return key(a) >= key(b) ? a : b; }
inline std::uint32_t min(std::uint32_t a, std::uint32_t b) { return key(a) <= key(b) ? a : b; }

inline std::uint32_t to_fixed(std::uint32_t x, int fraction) {
    const int e = static_cast<int>(exponent(x)) + fraction;
    if (e <= 126 || !exponent(x)) return 0;
    const bool negative = x & kSign;
    if (e >= 158) return negative ? 0x80000000u : 0x7FFFFFFFu;
    const int shift = e - 150;
    const std::uint32_t m = mantissa(x);
    const std::uint32_t v = shift >= 0 ? m << shift : m >> -shift;
    return negative ? 0u - v : v;
}

inline std::uint32_t from_fixed(std::int32_t v, int fraction) {
    return from_double(static_cast<double>(v) / static_cast<double>(1u << fraction));
}

inline std::uint32_t cvt_w_s(std::uint32_t x) { return to_fixed(x, 0); }
inline std::uint32_t cvt_s_w(std::uint32_t v) { return from_fixed(static_cast<std::int32_t>(v), 0); }

}

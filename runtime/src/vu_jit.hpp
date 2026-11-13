#pragma once

#include <cstdint>
#include <vector>

namespace ps2::vu::jit {

struct Layout {
    std::uint32_t flags = 0;
    // The caller runs the code with flush-to-zero on (ARM64 FPCR.FZ, see vu_micro.cpp run_entry): float
    // results are never denormal, so the clamp after arithmetic only has to cap the magnitude.
    bool ftz = false;
    // Unit shape: VU1 = 16 KB code / 1024 data qwords; VU0 = 4 KB / 256. keep_flags: every MAC / status /
    // clip update is kept (VU0: the EE reads them after VCALLMS with CFC2, outside the program).
    std::uint32_t code_mask = 16383u;
    std::uint32_t data_qwords = 1024u;
    bool keep_flags = false;
    // exact: upper arithmetic (add/sub/mul/madd/msub/opmula/opmsub) through the kExact import = the
    // interpreter's PS2 float routines (fp::add / fp::mul: chopped, one guard bit), on raw register values.
    // VU0 needs it (Ridge Racer V's VU0 code gave visibly different results with native floats). The
    // operands go through the 64-byte scratch at offset `scratch` (s, t, acc in; result at +48).
    bool exact = false;
    std::uint32_t scratch = 0;
};

enum Import : std::uint32_t {
    kMac, kStatus, kClip, kForceMac, kEfu, kKick, kTop, kItop, kInterpret, kExact, kImports
};

extern const char *const kImportNames[kImports];

bool translate(const std::uint8_t *code, std::uint32_t entry, const Layout &layout, std::vector<std::uint8_t> &out);

}

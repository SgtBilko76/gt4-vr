#pragma once

#include <cstdint>
#include <vector>

namespace ps2::vu::jit {

struct Layout {
    std::uint32_t flags = 0;
    // The caller runs the code with flush-to-zero on (ARM64 FPCR.FZ, see vu_micro.cpp run_entry): float
    // results are never denormal, so the clamp after arithmetic only has to cap the magnitude.
    bool ftz = false;
};

enum Import : std::uint32_t {
    kMac, kStatus, kClip, kForceMac, kEfu, kKick, kTop, kItop, kInterpret, kImports
};

extern const char *const kImportNames[kImports];

bool translate(const std::uint8_t *code, std::uint32_t entry, const Layout &layout, std::vector<std::uint8_t> &out);

}

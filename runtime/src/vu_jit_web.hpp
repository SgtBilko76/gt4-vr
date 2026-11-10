#pragma once

#include "ps2/vu_ops.hpp"

#include <cstdint>
#include <vector>

namespace ps2::vu::jit {

int mode();
ProgramFn compile(const std::uint8_t *code, std::uint32_t entry);
ProgramFn compile_a64(const std::uint8_t *code, std::uint32_t entry);  // vu_jit_a64.cpp (ARM64 only)
std::uintptr_t a64_base();
void *a64_flags();
const void *a64_import(std::uint32_t i);

extern bool g_dry;
extern std::int64_t g_escape;
extern std::vector<std::uint32_t> *g_kicks;

}

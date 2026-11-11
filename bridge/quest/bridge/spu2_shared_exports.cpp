// SPDX-License-Identifier: GPL-3.0-or-later
//
// librrv_pcsx2_spu2.so: the static rrv_pcsx2_spu2 archive (tools/pcsx2-spu2) as a shared
// library for Android. The archive's object files are only pulled into a link by a
// reference, so this table names every function of the C ABI; spu2.version then exports
// exactly those (rrv_spu2_*) and keeps PCSX2's SPU2 globals and the C++ runtime local.
#include "rrv_pcsx2_spu2.h"

namespace
{
using AnyFn = void (*)();
[[gnu::used]] const AnyFn kRrvSpu2Abi[] = {
    reinterpret_cast<AnyFn>(&rrv_spu2_create),
    reinterpret_cast<AnyFn>(&rrv_spu2_destroy),
    reinterpret_cast<AnyFn>(&rrv_spu2_write16),
    reinterpret_cast<AnyFn>(&rrv_spu2_read16),
    reinterpret_cast<AnyFn>(&rrv_spu2_dma_write),
    reinterpret_cast<AnyFn>(&rrv_spu2_dma_read),
    reinterpret_cast<AnyFn>(&rrv_spu2_advance),
    reinterpret_cast<AnyFn>(&rrv_spu2_sync),
    reinterpret_cast<AnyFn>(&rrv_spu2_pull_output),
    reinterpret_cast<AnyFn>(&rrv_spu2_get_stats),
    reinterpret_cast<AnyFn>(&rrv_spu2_state_hash),
};
} // namespace

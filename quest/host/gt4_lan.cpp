// GT4 Spec II 1.10: shows the Arcade menu's Multi-LAN Race entry again (owner 2026-11-06: network
// mode for Spec II, only with other Spec II 1.10 players - the relay groups them by product code).
//
// Spec II keeps GT4 Online's whole LAN code; its arcade script only hides the entry
// (GameRoot onInitialize: `LANBattle.visible = false; LANBattleBG.visible = false;`, the original
// sets both from !disable_lanbattle = true). Compiled, each line ends in the constant `0c 00`
// (false). The user's disc stays untouched: when the game decrypts projects/arcade/arcade.adc from
// GT4.VOL, the two bytes are set to 01 in RAM before the script is parsed.
//
// Hook (games/gt4s/probes.txt): GT4 streams files out of GT4.VOL through its inflater (0x529E90 ->
// inner loop 0x529800). The inflater writes into a small output buffer and hands it on through the
// stream's virtual "output full" call (vtable +0x18), right after storing the buffer start/end at
// stream +0x14 / +0x18. Five such call sites: 0x529A44 / 0x529D84 / 0x529E04 (inner inflate, stream
// in s0) and 0x52AC5C / 0x52AEE4 (stream in s2); with only the first, 21 of the file's pieces showed. Each piece is searched there; the last 64 bytes of
// the previous piece of the same stream are kept, so a line split across pieces is still found (its
// final byte, the one changed, is always in the newer piece). Measured: arcade.adc is never whole in
// RAM, so a scan after loading would miss the lines.
#include "gt4_lan.h"

#include "ps2/cpu.hpp"
#include "ps2/memory.hpp"
#include "ps2/system.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <string>

namespace gt4::lan {
namespace {

struct OutputSite { std::uint32_t pc; int stream_reg; };
constexpr OutputSite kOutputFull[] = {{0x529A44u, 16}, {0x529D84u, 16}, {0x529E04u, 16}, {0x52AC5Cu, 18}, {0x52AEE4u, 18}};
constexpr std::uint32_t kOutBegin = 0x14u, kOutEnd = 0x18u;
constexpr std::uint32_t kOverlap = 64;
constexpr std::uint32_t kMaxChunk = 1u << 20;
std::map<std::uint32_t, std::string> g_tail;  // per stream object
int g_patched = 0;
bool g_log = false;

std::string pattern(const char *widget, std::uint8_t widget_index, std::uint8_t line)
{
    // <len16>name <index32> | line32 03 (attribute) <count32=1> <len16>"visible" | line32 0c (bool) 00
    std::string p;
    const auto put16 = [&](std::uint16_t v) { p.push_back(static_cast<char>(v)); p.push_back(static_cast<char>(v >> 8)); };
    const auto put32 = [&](std::uint32_t v) { for (int i = 0; i < 4; ++i) p.push_back(static_cast<char>(v >> (8 * i))); };
    put16(static_cast<std::uint16_t>(std::strlen(widget)));
    p += widget;
    put32(widget_index);
    put32(line);
    p.push_back('\x03');
    put32(1);
    put16(7);
    p += "visible";
    put32(line);
    p.push_back('\x0c');
    p.push_back('\x00');
    return p;
}

void scan(std::uint32_t stream, std::uint32_t from, std::uint32_t to)
{
    static const std::string lines[2] = {pattern("LANBattle", 7, 0x79), pattern("LANBattleBG", 8, 0x7A)};
    if (to <= from || to - from > kMaxChunk || to > ps2::kRamSize) return;
    const auto *ram = reinterpret_cast<const char *>(ps2::g_ram);
    std::string &tail = g_tail[stream];
    std::string data = tail;
    data.append(ram + from, to - from);
    for (const std::string &p : lines)
    {
        const auto hit = data.find(p);
        if (hit == std::string::npos) continue;
        const std::size_t last = hit + p.size() - 1;  // index of the bool byte in data
        if (last < tail.size()) continue;            // already seen in the previous piece
        const auto at = static_cast<std::uint32_t>(from + (last - tail.size()));
        ps2::st8(at, 1);
        ++g_patched;
        std::fprintf(stderr, "[gt4lan] Multi-LAN Race shown: %s line at 0x%08X\n", p.c_str() + 2, at);
    }
    tail = data.size() > kOverlap ? data.substr(data.size() - kOverlap) : data;
}

} // namespace

void installSpecIIMenu()
{
    g_log = std::getenv("PS2_GT4_LANLOG") != nullptr;
    for (const OutputSite &site : kOutputFull)
    ps2::set_probe_hook(site.pc, [reg = site.stream_reg](ps2::Cpu &c) {
        const std::uint32_t stream = c.r[reg].w[0];
        const std::uint32_t from = ps2::ld32(stream + kOutBegin);
        const std::uint32_t to = ps2::ld32(stream + kOutEnd);
        if (g_log)
        {
            const auto *ram = reinterpret_cast<const char *>(ps2::g_ram);
            const bool has = to > from && to <= ps2::kRamSize && to - from <= kMaxChunk &&
                             memmem(ram + from, to - from, "LANBattle", 9);
            std::fprintf(stderr, "[gt4lan] piece %08X: %08X..%08X%s\n", stream, from, to, has ? " LANBattle" : "");
        }
        scan(stream, from, to);
    });
    std::fprintf(stderr, "[gt4lan] Spec II Multi-LAN entry: %zu inflate output hooks\n", std::size(kOutputFull));
}

} // namespace gt4::lan

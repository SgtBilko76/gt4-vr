// GT4: display mode 480p progressive by default.
//
// GT4 keeps one display-mode request object (retail 0x6D6708, A-Spec II 0x734848). Its request function
// (retail 0x100BB8, Spec II 0x1004F8) stores the wanted mode at +0x9C and sets +0xA0 = 1; the display
// object's update (vtable method retail 0x100A40, Spec II 0x100380) applies it, clears +0xA0 and leaves
// +0x9C as the current mode. Modes (jump table retail 0x68BB30, Spec II 0x6E0E30):
//   1 = 512x448 interlaced, 2 = 640x448 interlaced (boot, video player), 3 = 640x448 other,
//   4 = 480p progressive (SetGsCrt 0, 0x50), 5 = 1080i (0x51).
// Boot requests 2 and the video player requests 2 (retail 0x3D8A9C), whatever the player chose in the
// options - so 480p was lost at every start and after videos. Here: when mode 2 is applied and stays,
// no video is decoding (IPU idle) and the boot is past its first screens, request 4 the same way GT4's
// request function does (its mutex/semaphore calls are skipped: this runs between guest frames on the
// emulation thread). PC test 2026-11-08: switching at boot frame ~34 (SCE screen) froze the game, so the
// first switch waits for PS2_GT4_480P_FROM frames (default 1200, the title screen).
// Modes 1, 3 and 5 are left alone (a player's 1080i choice stays). PS2_GT4_480P=0 = off.
// Spec II also resets the GS to interlaced WITHOUT going through the request object (owner 2026-11-08 log:
// SetGsCrt 1/0x2 while +0x9C still said 4), so the actual SetGsCrt state is watched too: mode 4 applied but
// the GS interlaced again -> request 4 once more.
#include "gt4_display.h"

#include "ps2/memory.hpp"
#include "ps2/video.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gt4::display {
namespace {

constexpr std::uint32_t kModeOffset = 0x9Cu, kPendingOffset = 0xA0u;
constexpr std::uint32_t kInterlaced = 2u, kProgressive = 4u;
constexpr int kSettleFrames = 30;
constexpr std::uint64_t kVideoIdleFrames = 60;

bool g_on = false;
std::uint32_t g_object = 0;
std::uint64_t g_from = 1200;
std::uint32_t g_last_mode = 0xFFFFFFFFu, g_last_pending = 0xFFFFFFFFu;
std::uint64_t g_frame = 0, g_last_ipu = 0, g_video_frame = 0;
bool g_video = false;
int g_stable = 0;
unsigned g_switches = 0, g_logged = 0;

std::uint32_t rd32(std::uint32_t a) { return ps2::load<std::uint32_t>(ps2::g_ram + (a & (ps2::kRamSize - 1))); }
void wr32(std::uint32_t a, std::uint32_t v) { std::memcpy(ps2::g_ram + (a & (ps2::kRamSize - 1)), &v, 4); }

} // namespace

void install(bool retail)
{
    const char *v = std::getenv("PS2_GT4_480P");
    g_on = !(v && v[0] == '0');
    if (const char *f = std::getenv("PS2_GT4_480P_FROM"))
        g_from = std::strtoull(f, nullptr, 10);
    g_object = retail ? 0x006D6708u : 0x00734848u;
    std::fprintf(stderr, "[gt4display] 480p progressive %s (%s, request object 0x%06X, from frame %llu)\n",
                 g_on ? "on" : "off (PS2_GT4_480P=0)", retail ? "retail v2.00" : "A-Spec II", g_object,
                 static_cast<unsigned long long>(g_from));
}

void tick()
{
    if (!g_on)
        return;
    ++g_frame;
    const std::uint64_t ipu = ps2::ipu_output_qwords();
    if (ipu != g_last_ipu)
        g_video_frame = g_frame;
    g_last_ipu = ipu;
    const bool video = g_frame - g_video_frame < kVideoIdleFrames && g_video_frame != 0;
    const std::uint32_t mode = rd32(g_object + kModeOffset);
    const std::uint32_t pending = rd32(g_object + kPendingOffset);
    if ((mode != g_last_mode || pending != g_last_pending || video != g_video) && g_logged < 80 && ++g_logged)
        std::fprintf(stderr, "[gt4display] frame %llu: mode %u%s, video %s\n", static_cast<unsigned long long>(g_frame), mode,
                     pending ? " requested" : " applied", video ? "playing" : "idle");
    g_last_mode = mode;
    g_last_pending = pending;
    g_video = video;
    const bool lost = mode == kProgressive && ps2::gs_crt_interlace() != 0;
    if (pending != 0 || (mode != kInterlaced && !lost) || video || g_frame < g_from)
    {
        g_stable = 0;
        return;
    }
    if (++g_stable < kSettleFrames)
        return;
    g_stable = 0;
    wr32(g_object + kModeOffset, kProgressive);
    wr32(g_object + kPendingOffset, 1u);
    if (++g_switches <= 20)
        std::fprintf(stderr, "[gt4display] frame %llu: %s -> 4 (480p progressive) requested (%u)\n",
                     static_cast<unsigned long long>(g_frame), lost ? "GS reset to interlaced behind mode 4" : "mode 2 (interlaced)", g_switches);
}

} // namespace gt4::display

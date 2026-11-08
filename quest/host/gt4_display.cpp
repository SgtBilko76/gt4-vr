// GT4: display mode 480p progressive while racing.
//
// GT4 keeps one display-mode request object (retail 0x6D6708, A-Spec II 0x734848). Its request function
// (retail 0x100BB8, Spec II 0x1004F8) stores the wanted mode at +0x9C and sets +0xA0 = 1; the display
// object's update (vtable method retail 0x100A40, Spec II 0x100380) applies it, clears +0xA0 and leaves
// +0x9C as the current mode. Modes (jump table retail 0x68BB30, Spec II 0x6E0E30):
//   1 = 512x448 interlaced, 2 = 640x448 interlaced (boot, video player), 3 = 640x448 other,
//   4 = 480p progressive (SetGsCrt 0, 0x50), 5 = 1080i (0x51).
// GT4 never applies the player's saved choice and requests 2 again after videos and scenes, so 480p was
// lost all the time. The VR picture is far sharper in 480p (owner 2026-11-08).
//
// Races only (PS2_GT4_480P=1, the default): the main menu's car scene draws a green block in 480p
// (owner screenshot 2026-11-08), so the menus stay interlaced. While the player drives (the race camera
// builds with a driving camera, as the XR host's VR switch uses) and mode 2 is applied with no video
// decoding, request 4 the way GT4's request function does (its mutex/semaphore calls are skipped: this
// runs between guest frames on the emulation thread). Spec II also resets the GS to interlaced behind
// mode 4 (SetGsCrt watched): then 4 is requested again. 2 s after driving stops (finish, replay,
// menus) mode 2 is requested back if this file had switched. PS2_GT4_480P=2 = always (menus too, from
// frame 1200 - switching in the first boot frames froze the game on the PC), 0 = off.
#include "gt4_display.h"

#include "gt4_camera.h"
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
constexpr std::uint64_t kRaceIdleFrames = 20;   // race camera builds this recent = the race is drawn
constexpr std::uint64_t kRaceOverFrames = 120;  // not driving this long = back to interlaced

int g_policy = 1; // 0 off, 1 races, 2 always
std::uint32_t g_object = 0;
std::uint64_t g_from = 1200;
std::uint32_t g_last_mode = 0xFFFFFFFFu, g_last_pending = 0xFFFFFFFFu;
std::uint64_t g_frame = 0, g_last_ipu = 0, g_video_frame = 0;
std::uint64_t g_last_builds = 0, g_build_frame = 0, g_drive_frame = 0;
bool g_video = false, g_driving = false, g_ours = false;
int g_stable = 0;
unsigned g_switches = 0, g_logged = 0;

std::uint32_t rd32(std::uint32_t a) { return ps2::load<std::uint32_t>(ps2::g_ram + (a & (ps2::kRamSize - 1))); }
void wr32(std::uint32_t a, std::uint32_t v) { std::memcpy(ps2::g_ram + (a & (ps2::kRamSize - 1)), &v, 4); }

void request(std::uint32_t mode, const char *why)
{
    wr32(g_object + kModeOffset, mode);
    wr32(g_object + kPendingOffset, 1u);
    if (++g_switches <= 40)
        std::fprintf(stderr, "[gt4display] frame %llu: %s -> mode %u requested (%u)\n", static_cast<unsigned long long>(g_frame), why,
                     mode, g_switches);
}

} // namespace

void install(bool retail)
{
    if (const char *v = std::getenv("PS2_GT4_480P"); v && v[0] >= '0' && v[0] <= '2')
        g_policy = v[0] - '0';
    if (const char *f = std::getenv("PS2_GT4_480P_FROM"))
        g_from = std::strtoull(f, nullptr, 10);
    g_object = retail ? 0x006D6708u : 0x00734848u;
    static const char *const kPolicy[] = {"off", "while racing", "always"};
    std::fprintf(stderr, "[gt4display] 480p progressive %s (PS2_GT4_480P=%d; %s, request object 0x%06X)\n", kPolicy[g_policy], g_policy,
                 retail ? "retail v2.00" : "A-Spec II", g_object);
}

void tick()
{
    if (g_policy == 0)
        return;
    ++g_frame;
    const std::uint64_t ipu = ps2::ipu_output_qwords();
    if (ipu != g_last_ipu)
        g_video_frame = g_frame;
    g_last_ipu = ipu;
    const bool video = g_video_frame != 0 && g_frame - g_video_frame < kVideoIdleFrames;

    // Driving: the race scene is built (menus build none) by the race renderer in its driving mode with
    // the player's camera - the same test as the XR host's VR switch (quest_xr.cpp).
    const std::uint64_t builds = gt4::camera::sceneBuilds();
    if (builds != g_last_builds)
        g_build_frame = g_frame;
    g_last_builds = builds;
    const gt4::camera::SceneState scene = gt4::camera::sceneState();
    const bool driving_now = g_build_frame != 0 && g_frame - g_build_frame < kRaceIdleFrames && scene.mode == 0 && !scene.windowed &&
                             scene.driver_camera;
    if (driving_now)
        g_drive_frame = g_frame;
    const bool driving = g_drive_frame != 0 && g_frame - g_drive_frame < kRaceOverFrames;

    const std::uint32_t mode = rd32(g_object + kModeOffset);
    const std::uint32_t pending = rd32(g_object + kPendingOffset);
    if ((mode != g_last_mode || pending != g_last_pending || video != g_video || driving != g_driving) && g_logged < 120 && ++g_logged)
        std::fprintf(stderr, "[gt4display] frame %llu: mode %u%s, video %s, %s\n", static_cast<unsigned long long>(g_frame), mode,
                     pending ? " requested" : " applied", video ? "playing" : "idle", driving ? "driving" : "not driving");
    g_last_mode = mode;
    g_last_pending = pending;
    g_video = video;
    g_driving = driving;
    if (pending != 0 || video)
    {
        g_stable = 0;
        return;
    }

    const bool want_progressive = g_policy == 2 ? g_frame >= g_from : driving;
    if (!want_progressive)
    {
        // Back to interlaced only if this file switched (a player's own 480p/1080i choice stays).
        g_stable = 0;
        if (g_ours && mode == kProgressive)
            request(kInterlaced, "not driving");
        g_ours = false;
        return;
    }
    const bool lost = mode == kProgressive && ps2::gs_crt_interlace() != 0;
    if (mode != kInterlaced && !lost)
    {
        g_stable = 0;
        return;
    }
    if (++g_stable < kSettleFrames)
        return;
    g_stable = 0;
    g_ours = true;
    request(kProgressive, lost ? "GS reset to interlaced behind mode 4" : (g_policy == 2 ? "interlaced" : "driving, interlaced"));
}

} // namespace gt4::display

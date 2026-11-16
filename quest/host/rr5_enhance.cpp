// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ridge Racer V (SLUS-20002) enhancements for the program read from the player's disc: real
// widescreen. The mechanism is the Fukami RR5 port's (src/product/patches.cpp, "RR5 enhancement:
// real widescreen"), rebuilt on the runtime's probe hooks: a hook runs before the guest
// instruction at its address, so a function ENTRY hook runs before the function and a hook on its
// `jr $ra` runs after its body.
//
// The camera block at 0x1DE0CB0 holds p[0] (screen distance), p[1] (X factor) and p[2] (Y
// factor); the projection is screen x = p1*p0*x/z, y = p2*p0*y/z. p[0] x(4/3)/A widens the view and
// p[2] xA/(4/3) keeps its height, so only the horizontal field of view grows, and everything
// derived from p[0] (VU1 clip constants, cull planes, ...) widens with it. The block is normalised
// at the entry of every function that reads it: a value the game wrote is scaled once, a value
// produced here is left alone (a ring of produced values). The car LOD selector (func_226EB0)
// gets the game's own p[0] back for its call, so car detail does not change.
// Intro (attract cinematic, func_23E3C8): already a wide letterboxed picture - p[0] is left alone,
// p[2] and the character projection's ay are scaled, and the two letterbox quads (func_23D408) get
// zero height; character vertices are kept inside the GS range. Main menu: the character's ax
// (func_2364F0 -> func_22CB88) is scaled like the camera, and the dot lattice (func_275AA8) is drawn
// once more 1024 units to the left so the wider view has no dot-less band.
//
// RRV_RR5_WIDESCREEN=16:9|16:10|21:9 (1 = 16:9) turns it on; RRV_RR5_WIDESCREEN_LOG=1 logs.
// Also here: the VR race view (installRaceView, see below).
#include "rr5_enhance.h"

#include "ps2/cpu.hpp"
#include "ps2/generated.hpp"
#include "ps2/guest_call.hpp"
#include "ps2/memory.hpp"
#include "ps2/system.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rr5::enhance
{
namespace
{
constexpr std::uint32_t kBlockP0 = 0x1DE0CB0u, kBlockP1 = 0x1DE0CB4u, kBlockP2 = 0x1DE0CB8u;
constexpr std::uint32_t kAttractP1Bits = 0x3F4CCCCDu; // 0.8f
constexpr std::uint32_t kViewScreenAx = 0x332F80u, kViewScreenAy = 0x332F84u;
constexpr std::uint32_t kPacketCursor = 0x3470A0u;
constexpr std::uint32_t kScenePhase = 0x334E94u;
constexpr std::uint32_t kReaders[] = {0x220E10u, 0x21BF08u, 0x21B408u, 0x21B098u, 0x222620u,
                                      0x2423E0u, 0x29F990u, 0x2A2EF0u, 0x2AAA60u};
// Function entries and their `jr $ra` (from the program's code; functions with one return).
constexpr std::uint32_t kLod = 0x226EB0u, kLodReturn = 0x22728Cu;
constexpr std::uint32_t kAttract = 0x23E3C8u, kAttractReturn = 0x23E660u;
constexpr std::uint32_t kBars = 0x23D408u, kBarsReturn = 0x23D6B4u;
constexpr std::uint32_t kMenuGirl = 0x2364F0u, kMenuGirlReturn = 0x2368B8u;
constexpr std::uint32_t kViewScreen = 0x22CB88u;
constexpr std::uint32_t kDots = 0x275AA8u;
constexpr std::uint32_t kDotsModel = 0x335030u;
constexpr float kDotsStep = 1024.0f;
struct CharTransform { std::uint32_t entry, ret; };
constexpr CharTransform kCharTransforms[] = {{0x22E7A8u, 0x22EAA0u}, {0x22EE78u, 0x22F364u},
                                             {0x22EAA8u, 0x22EE6Cu}, {0x22F370u, 0x22F8F0u}};

float f32(std::uint32_t a) { return std::bit_cast<float>(ps2::ld32(a)); }
void setf32(std::uint32_t a, float v) { ps2::st32(a, std::bit_cast<std::uint32_t>(v)); }

struct Produced {
    std::uint32_t scaled[16] = {}, orig[16] = {};
    std::uint32_t next = 0;
    bool find(std::uint32_t v, std::uint32_t *o) const {
        for (std::uint32_t i = 0; i < 16u; ++i)
            if (scaled[i] == v && v != 0u) { if (o) *o = orig[i]; return true; }
        return false;
    }
    void add(std::uint32_t s, std::uint32_t o) { scaled[next] = s; orig[next] = o; next = (next + 1u) % 16u; }
};
Produced g_p0, g_p2;
float g_factor = 0.75f; // (4/3) / target aspect
bool g_log = false;
int g_attract_depth = 0, g_menu_girl_depth = 0;
bool g_intro = false;
std::uint32_t g_intro_p0 = 0;

void normalize_one(std::uint32_t addr, Produced &produced, float factor) {
    const std::uint32_t bits = ps2::ld32(addr);
    if (bits == 0u || produced.find(bits, nullptr)) return;
    const std::uint32_t s = std::bit_cast<std::uint32_t>(std::bit_cast<float>(bits) * factor);
    ps2::st32(addr, s);
    produced.add(s, bits);
}

bool intro_profile(std::uint32_t reader, std::uint32_t ra) {
    const std::uint32_t p0 = ps2::ld32(kBlockP0), p1 = ps2::ld32(kBlockP1);
    const bool intro = g_attract_depth > 0 || (g_intro && p0 == g_intro_p0 && p1 == kAttractP1Bits);
    if (intro) g_intro_p0 = p0;
    if (g_log && intro != g_intro)
        std::fprintf(stderr, "[rr5-ws] profile %s (scene %u, p %.3f %.4f %.4f, reader %06X ra %06X)\n",
                     intro ? "intro" : "gameplay", ps2::ld32(kScenePhase), f32(kBlockP0), f32(kBlockP1), f32(kBlockP2),
                     reader, ra);
    g_intro = intro;
    return intro;
}

void normalize(std::uint32_t reader, std::uint32_t ra) {
    if (!intro_profile(reader, ra)) normalize_one(kBlockP0, g_p0, g_factor);
    normalize_one(kBlockP2, g_p2, 1.0f / g_factor);
}

// LOD selector: the game's p[0] for its call, the scaled one back after it.
std::uint32_t g_lod_scaled = 0, g_lod_orig = 0;

// func_22CB88 is the only reader of ax / ay: each call gets the stock value or the scaled one.
std::uint32_t g_ax_stock = 0, g_ay_stock = 0, g_ax_written = 0, g_ay_written = 0;
bool g_ax_seen = false, g_ay_seen = false;
void viewscreen_word(std::uint32_t addr, std::uint32_t &stock, std::uint32_t &written, bool &seen, float scale) {
    const std::uint32_t cur = ps2::ld32(addr);
    if (!seen || cur != written) { stock = cur; seen = true; } // the game (re)wrote it
    written = scale == 1.0f ? stock : std::bit_cast<std::uint32_t>(std::bit_cast<float>(stock) * scale);
    ps2::st32(addr, written);
}

// Letterbox: GIF tag 0x5122400000008001 (one tristrip), registers 0x55551, Y at +0x24/+0x34 and +0x44/+0x54.
bool collapse_bar(std::uint32_t packet, std::uint32_t y0, std::uint32_t y1, std::uint32_t y) {
    if (ps2::ld64(packet) != 0x5122400000008001ull || ps2::ld64(packet + 8u) != 0x55551ull) return false;
    if (ps2::ld32(packet + 0x24u) != y0 || ps2::ld32(packet + 0x34u) != y0) return false;
    if (ps2::ld32(packet + 0x44u) != y1 || ps2::ld32(packet + 0x54u) != y1) return false;
    for (const std::uint32_t off : {0x24u, 0x34u, 0x44u, 0x54u}) ps2::st32(packet + off, y);
    return true;
}
std::uint32_t g_bars_first = 0;
bool g_bars_armed = false;

struct CharCall { std::uint32_t part, positions; bool armed; };
CharCall g_char[4] = {};
std::uint32_t g_char_clamped = 0;
bool g_ws_on = false;

bool g_in_dots = false;
std::uint64_t g_dots_runs = 0, g_dots_failed = 0;

// ---------------------------------------------------------------------------------------------
// VR race view (the Fukami app's quest/host/quest_headtrack.cpp on probe hooks).
// The game builds every 3D camera in func_220E10(block = 0x1DE0CB0, camera): projection P
// (0x1DE0E30) from the block, view V (0x1DE0CE0), then tail-calls sceVu0MulMatrix D = P * V
// (0x1DE0D20). libvu0 matrices: four 16-byte rows in memory, each a COLUMN. Camera space is x right,
// y down, z forward; an OpenXR head rotation q = (x, y, z, w) is (x, -y, -z, w) there. Looking around
// is V' = R^T V, D' = P V'. A race builds four cameras per frame, all returning to the callers below
// (2125D8 = the driving view, it comes first); menus, intro and attract use other callers and stay
// the game's. Race phase word 0x1DC0140: 0 start cinematic, 1 countdown, 2 GO, 3 racing.
// Immersive (the head pose carries tanX / tanY): the block gets p0 = 320 / (p1 tanX), p2 = 112 /
// (tanY p0) for every race camera of the frame (the game rewrites p2 between the builds), entered in
// the widescreen rings as produced values; the picture window is moved by shiftY like GT4's
// (gt4_camera.cpp): screen y' = y - 112 shiftY / tanY.
constexpr std::uint32_t kCameraBuild = 0x220E10u;
constexpr std::uint32_t kViewMatrix = 0x1DE0CE0u, kViewProj = 0x1DE0D20u, kProjection = 0x1DE0E30u;
constexpr std::uint32_t kGameplayP1 = 0x3F800000u;
constexpr float kHalfWidthUnits = 320.0f, kHalfHeightUnits = 112.0f;
constexpr std::uint32_t kRaceMainCaller = 0x2125D8u;
constexpr std::uint32_t kRaceCallers[] = {0x2125D8u, 0x21D1D8u, 0x2A2F54u, 0x2215ACu};
constexpr std::uint32_t kRaceHold = 16u;
constexpr std::uint32_t kRacePhase = 0x1DC0140u;

bool g_view_on = false;
gt4::camera::HeadSource g_head = nullptr;
gt4::camera::FrameSink g_frame = nullptr;
std::uint32_t g_builds_since_race = kRaceHold;
std::uint32_t g_race_phase = ~0u;
bool g_immersive = false;
float g_imm_p0 = 0.0f, g_imm_p2 = 0.0f;
std::atomic<std::uint64_t> g_race_builds{0};
struct Slot { std::uint32_t mine = 0, normal = 0; };
Slot g_slot_p0, g_slot_p2;
// The build in flight (entry -> its return address).
bool g_call_tracked = false;
std::uint32_t g_call_ra = 0;
gt4::camera::Head g_call_head;

void set_one(std::uint32_t addr, Produced &ring, Slot &slot, bool on, float value) {
    const std::uint32_t cur = ps2::ld32(addr);
    if (slot.mine == 0u || cur != slot.mine) slot.normal = cur;
    if (!on) {
        if (slot.mine != 0u && cur == slot.mine) ps2::st32(addr, slot.normal);
        slot.mine = 0u;
        return;
    }
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    if (g_ws_on && !ring.find(bits, nullptr)) {
        std::uint32_t orig = slot.normal, o;
        if (ring.find(slot.normal, &o)) orig = o;
        ring.add(bits, orig);
    }
    ps2::st32(addr, bits);
    slot.mine = bits;
}
void race_projection(bool on, float p0, float p2) {
    set_one(kBlockP0, g_p0, g_slot_p0, on, p0);
    set_one(kBlockP2, g_p2, g_slot_p2, on, p2);
}

void race_entry(ps2::Cpu &c) {
    const std::uint32_t block = c.r[4].w[0];
    const std::uint32_t ra = c.r[31].w[0];
    g_call_tracked = false;
    if (block != kBlockP0) return;
    if (ra == kRaceMainCaller) {
        g_builds_since_race = 0u;
        const std::uint32_t phase = ps2::ld32(kRacePhase);
        if (phase != g_race_phase) {
            g_race_phase = phase;
            std::fprintf(stderr, "[rr5-vr] race phase %u (p0 %.2f p1 %.4f p2 %.4f): %s\n", phase, f32(kBlockP0), f32(kBlockP1),
                         f32(kBlockP2), phase >= 1u && phase <= 3u ? "driving view" : "flat");
        }
    } else if (g_builds_since_race < kRaceHold)
        ++g_builds_since_race;
    bool race_caller = false;
    for (const std::uint32_t r : kRaceCallers) race_caller = race_caller || ra == r;
    const bool driving = g_race_phase >= 1u && g_race_phase <= 3u;
    const bool p1_gameplay = ps2::ld32(kBlockP1) == kGameplayP1;
    const bool tracked = p1_gameplay && race_caller && g_builds_since_race < kRaceHold && driving;
    if (ra == kRaceMainCaller && p1_gameplay && driving) g_race_builds.fetch_add(1, std::memory_order_relaxed);
    const gt4::camera::Head head = tracked && g_head ? g_head() : gt4::camera::Head{};
    if (ra != kRaceMainCaller && g_immersive && tracked) race_projection(true, g_imm_p0, g_imm_p2);
    if (ra == kRaceMainCaller) {
        const bool immersive = tracked && head.key != 0 && head.tanX > 0.05f && head.tanY > 0.05f;
        const float p1 = f32(kBlockP1);
        const float p0 = immersive ? kHalfWidthUnits / (p1 * head.tanX) : 0.0f;
        const float p2 = immersive ? kHalfHeightUnits / (head.tanY * p0) : 0.0f;
        g_imm_p0 = p0;
        g_imm_p2 = p2;
        race_projection(immersive, p0, p2);
        if (immersive != g_immersive)
            std::fprintf(stderr, "[rr5-vr] race camera: %s (tan %.3f x %.3f, shift %.3f, p0 %.2f p2 %.4f)\n",
                         immersive ? "immersive" : "game projection", head.tanX, head.tanY, head.shiftY, p0, p2);
        g_immersive = immersive;
    }
    g_call_tracked = tracked && head.key != 0;
    g_call_ra = ra;
    g_call_head = head;
}

void read_matrix(std::uint32_t a, float m[4][4]) {
    for (int k = 0; k < 4; ++k)
        for (int i = 0; i < 4; ++i) m[k][i] = f32(a + 16u * k + 4u * i);
}
void write_matrix(std::uint32_t a, const float m[4][4]) {
    for (int k = 0; k < 4; ++k)
        for (int i = 0; i < 4; ++i) setf32(a + 16u * k + 4u * i, m[k][i]);
}

void race_after(std::uint32_t ra) {
    if (!g_call_tracked || g_call_ra != ra) return;
    g_call_tracked = false;
    const gt4::camera::Head &head = g_call_head;
    float v[4][4], p[4][4];
    read_matrix(kViewMatrix, v);
    read_matrix(kProjection, p);
    const float x = head.qx, y = -head.qy, z = -head.qz, w = head.qw; // OpenXR -> camera space
    const float r[3][3] = {
        {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
        {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
        {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)},
    };
    for (int k = 0; k < 4; ++k) {
        const float c0 = v[k][0], c1 = v[k][1], c2 = v[k][2];
        for (int i = 0; i < 3; ++i) v[k][i] = r[0][i] * c0 + r[1][i] * c1 + r[2][i] * c2;
    }
    float d[4][4];
    for (int k = 0; k < 4; ++k)
        for (int i = 0; i < 4; ++i) d[k][i] = p[0][i] * v[k][0] + p[1][i] * v[k][1] + p[2][i] * v[k][2] + p[3][i] * v[k][3];
    if (g_immersive && head.shiftY != 0.0f && head.tanY > 0.05f) {
        const float s = kHalfHeightUnits * head.shiftY / head.tanY;
        for (int k = 0; k < 4; ++k) d[k][1] -= s * d[k][3];
    }
    write_matrix(kViewMatrix, v);
    write_matrix(kViewProj, d);
    const float p0 = f32(kBlockP0), p1 = f32(kBlockP1), p2 = f32(kBlockP2);
    const float tan_x = (p0 > 0.0f && p1 > 0.0f) ? kHalfWidthUnits / (p1 * p0) : 0.0f;
    const float tan_y = (p0 > 0.0f && p2 > 0.0f) ? kHalfHeightUnits / (p2 * p0) : 0.0f;
    if (ra == kRaceMainCaller && g_frame) g_frame(head.key, tan_x, tan_y, g_immersive);
    static float logged_tan_x = -1.0f;
    if (ra == kRaceMainCaller && std::fabs(tan_x - logged_tan_x) > 0.01f) {
        logged_tan_x = tan_x;
        std::fprintf(stderr, "[rr5-vr] race camera field of view %.1f x %.1f deg (p0 %.2f p1 %.3f p2 %.4f)\n",
                     2.0f * std::atan(tan_x) * 57.29578f, 2.0f * std::atan(tan_y) * 57.29578f, p0, p1, p2);
    }
}

// func_220E10's entry: the race view first (it sets the block), then the widescreen normaliser
// (which leaves the values produced here alone) - the Fukami app's wrapper order.
void camera_hook_refresh() {
    if (!g_ws_on && !g_view_on) return;
    ps2::set_probe_hook(kCameraBuild, [](ps2::Cpu &c) {
        if (g_view_on) race_entry(c);
        if (g_ws_on) normalize(kCameraBuild, c.r[31].w[0]);
    });
}

// ps2run: PS2_RR5_TEST_YAW / _PITCH (degrees, positive = left / up), PS2_RR5_TEST_TAN "tx,ty",
// PS2_RR5_TEST_SHIFT (tan).
gt4::camera::Head test_head() {
    gt4::camera::Head h;
    const char *yaw = std::getenv("PS2_RR5_TEST_YAW");
    const char *pitch = std::getenv("PS2_RR5_TEST_PITCH");
    const float ay = (yaw ? std::strtof(yaw, nullptr) : 0.0f) * 0.0174533f * 0.5f;
    const float ap = (pitch ? std::strtof(pitch, nullptr) : 0.0f) * 0.0174533f * 0.5f;
    const float cy = std::cos(ay), sy = std::sin(ay), cp = std::cos(ap), sp = std::sin(ap);
    h.qw = cy * cp;
    h.qx = cy * sp;
    h.qy = sy * cp;
    h.qz = -sy * sp;
    h.key = 1;
    if (const char *t = std::getenv("PS2_RR5_TEST_TAN")) std::sscanf(t, "%f,%f", &h.tanX, &h.tanY);
    if (const char *s = std::getenv("PS2_RR5_TEST_SHIFT")) h.shiftY = std::strtof(s, nullptr);
    return h;
}
} // namespace

void installRaceView(gt4::camera::HeadSource head, gt4::camera::FrameSink frame)
{
    g_head = head;
    if (!g_head && (std::getenv("PS2_RR5_TEST_YAW") || std::getenv("PS2_RR5_TEST_PITCH") || std::getenv("PS2_RR5_TEST_TAN")))
        g_head = test_head;
    g_frame = frame;
    g_view_on = true;
    camera_hook_refresh();
    for (const std::uint32_t ra : kRaceCallers)
        ps2::set_probe_hook(ra, [ra](ps2::Cpu &) { race_after(ra); });
    std::fprintf(stderr, "[rr5-vr] race view hooks on func_220E10 and its race callers\n");
}

std::uint64_t raceBuilds() { return g_race_builds.load(std::memory_order_relaxed); }

bool installWidescreen()
{
    const char *e = std::getenv("RRV_RR5_WIDESCREEN");
    if (!e || !e[0] || std::strcmp(e, "0") == 0 || std::strcmp(e, "4:3") == 0) return false;
    float aspect = 0.0f;
    if (std::strcmp(e, "1") == 0 || std::strcmp(e, "16:9") == 0) aspect = 16.0f / 9.0f;
    else if (std::strcmp(e, "16:10") == 0) aspect = 16.0f / 10.0f;
    else if (std::strcmp(e, "21:9") == 0) aspect = 21.0f / 9.0f;
    if (aspect <= 0.0f) {
        std::fprintf(stderr, "[rr5-ws] ignoring RRV_RR5_WIDESCREEN=%s (want 16:9, 16:10 or 21:9)\n", e);
        return false;
    }
    g_factor = (4.0f / 3.0f) / aspect;
    const char *log = std::getenv("RRV_RR5_WIDESCREEN_LOG");
    g_log = log && log[0] && log[0] != '0';

    g_ws_on = true;
    for (const std::uint32_t reader : kReaders)
        ps2::set_probe_hook(reader, [reader](ps2::Cpu &c) { normalize(reader, c.r[31].w[0]); });
    camera_hook_refresh();

    // The LOD selector reads p[0] (it is one of the readers too).
    ps2::set_probe_hook(kLod, [](ps2::Cpu &c) {
        normalize(kLod, c.r[31].w[0]);
        g_lod_scaled = 0;
        std::uint32_t orig = 0;
        const std::uint32_t cur = ps2::ld32(kBlockP0);
        if (!g_intro && g_p0.find(cur, &orig)) {
            ps2::st32(kBlockP0, orig);
            g_lod_scaled = cur;
            g_lod_orig = orig;
        }
    });
    ps2::set_probe_hook(kLodReturn, [](ps2::Cpu &) {
        if (g_lod_scaled && ps2::ld32(kBlockP0) == g_lod_orig) ps2::st32(kBlockP0, g_lod_scaled);
        g_lod_scaled = 0;
    });

    // Intro profile.
    ps2::set_probe_hook(kAttract, [](ps2::Cpu &) { ++g_attract_depth; });
    ps2::set_probe_hook(kAttractReturn, [](ps2::Cpu &) { if (g_attract_depth > 0) --g_attract_depth; });
    ps2::set_probe_hook(kBars, [](ps2::Cpu &) {
        g_bars_armed = g_attract_depth > 0;
        g_bars_first = ps2::ld32(kPacketCursor);
    });
    ps2::set_probe_hook(kBarsReturn, [](ps2::Cpu &) {
        if (!g_bars_armed) return;
        g_bars_armed = false;
        const bool top = collapse_bar(g_bars_first, 0x7900u, 0x7A70u, 0x7900u);
        const bool bottom = collapse_bar(g_bars_first + 0x80u, 0x85B0u, 0x8700u, 0x8700u);
        static bool reported = false;
        if ((!top || !bottom) && !reported) {
            reported = true;
            std::fprintf(stderr, "[rr5-ws] intro: letterbox packets not found at 0x%08X (top %d, bottom %d)\n",
                         g_bars_first, top, bottom);
        }
    });
    for (std::uint32_t i = 0; i < 4u; ++i) {
        const CharTransform t = kCharTransforms[i];
        ps2::set_probe_hook(t.entry, [i](ps2::Cpu &c) {
            g_char[i] = {c.r[5].w[0], c.r[6].w[0], g_attract_depth > 0};
        });
        ps2::set_probe_hook(t.ret, [i, t](ps2::Cpu &) {
            const CharCall call = g_char[i];
            g_char[i].armed = false;
            if (!call.armed) return;
            const std::uint32_t count = ps2::ld16(call.part + 0x6Eu);
            if (count > 4096u) return;
            for (std::uint32_t k = 0; k < count; ++k) {
                const std::uint32_t v = call.positions + k * 16u;
                const std::int32_t x = static_cast<std::int32_t>(ps2::ld32(v)), y = static_cast<std::int32_t>(ps2::ld32(v + 4u));
                const std::int32_t cx = std::clamp(x, 0, 0xFFFF), cy = std::clamp(y, 0, 0xFFFF);
                if (cx == x && cy == y) continue;
                ps2::st32(v, static_cast<std::uint32_t>(cx));
                ps2::st32(v + 4u, static_cast<std::uint32_t>(cy));
                if (g_log && g_char_clamped++ == 0u)
                    std::fprintf(stderr, "[rr5-ws] intro: character vertex clamped to the GS range (func_%06X)\n", t.entry);
            }
        });
    }

    // Main-menu character and the attract's character projection (func_22CB88).
    ps2::set_probe_hook(kMenuGirl, [](ps2::Cpu &) { ++g_menu_girl_depth; });
    ps2::set_probe_hook(kMenuGirlReturn, [](ps2::Cpu &) { if (g_menu_girl_depth > 0) --g_menu_girl_depth; });
    ps2::set_probe_hook(kViewScreen, [](ps2::Cpu &) {
        const bool attract = g_attract_depth > 0;
        const bool menu = !attract && g_menu_girl_depth > 0;
        viewscreen_word(kViewScreenAx, g_ax_stock, g_ax_written, g_ax_seen, menu ? g_factor : 1.0f);
        viewscreen_word(kViewScreenAy, g_ay_stock, g_ay_written, g_ay_seen, attract ? 1.0f / g_factor : 1.0f);
    });

    // Main-menu dot lattice: one run at X -1024 first, then the game's own run.
    ps2::set_probe_hook(kDots, [](ps2::Cpu &c) {
        if (g_in_dots) return;
        const std::uint32_t model = ps2::ld32(kDotsModel);
        if (model < 0x100000u || model >= 0x2000000u) return;
        const std::uint32_t tx = model + 0x10u;
        const std::uint32_t ra = c.r[31].w[0];
        const std::uint32_t stock = ps2::ld32(tx);
        const ps2::Cpu saved = c;
        setf32(tx, std::bit_cast<float>(stock) - kDotsStep);
        g_in_dots = true;
        ps2::guest_call_hold_begin(c);
        ps2::call(c, kDots, 1u);
        ps2::guest_call_hold_end(c);
        g_in_dots = false;
        const bool ok = c.pc == ra;
        const std::int32_t budget = c.budget;
        c = saved;
        c.budget = budget;
        ps2::st32(tx, stock);
        if (ok) ++g_dots_runs;
        else if (g_dots_failed++ == 0)
            std::fprintf(stderr, "[rr5-ws] menu dots: extra run did not return (pc %08X, want %08X)\n", c.pc, ra);
    });

    std::fprintf(stderr, "[rr5-ws] widescreen %s: camera p[0] x%.4f, p[2] x%.4f at %zu readers, intro profile, "
                         "menu character and dots\n", e, g_factor, 1.0f / g_factor, std::size(kReaders));
    return true;
}

} // namespace rr5::enhance

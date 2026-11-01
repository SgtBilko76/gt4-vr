// SPDX-License-Identifier: GPL-3.0-or-later
//
// GT4 (A-Spec II, SCUS-97436) race camera hooks, see gt4_camera.h.
//
// GT4 draws through "pgl", an OpenGL-like library on VU0 macro mode: the current matrix lives in
// VF28..VF31 (one register per COLUMN, multMatrix 0x5A41A0 does C = C * M), pglMatrixMode
// 0x5A1ED8 (1 = projection, 0 = modelview), pglLoadIdentity 0x5A3F5C. The race scene is set up in
// the race renderer (around 0x1EB1C0, measured in A-Spec II with probe counts on the Nurburgring):
//   matrixMode(1); loadIdentity; camera->applyProjection(f20)   -> returns to 0x1EB1F4
//   multMatrix(renderer + 0xE434); ...
//   matrixMode(0); loadIdentity; camera->applyModelview(f20)    -> returns to 0x1EB234
//   0x29E038(scene)                                             draws the race
// The camera object is the race camera manager (vtable 0x6B5188: applyProjection 0x2378E8,
// applyModelview 0x2379A0). Other users of the manager (dynamic envmap 0x2EA5A8, 0x1E8C0C,
// getModelviewMatrix 0x31DD50) keep the game's view.
//
// Hooks run before the guest instruction at the probe address, i.e. right after the call returned:
//   0x1EB1F4: C = P. Widen it: rows 0/1 (x and y of every column) scaled so the projection's
//             tan(half fov) becomes the headset's.
//   0x1EB234: C = V. Turn it: C = R^T * V (R = head rotation in GT4's eye space).
#include "gt4_camera.h"

#include "ps2/cpu.hpp"
#include "ps2/generated.hpp"
#include "ps2/guest_call.hpp"
#include "ps2/system.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gt4::camera
{
namespace
{
constexpr std::uint32_t kAfterProjection = 0x1EB1F4u;
constexpr std::uint32_t kAfterModelview = 0x1EB234u;

HeadSource g_head = nullptr;
FrameSink g_frame = nullptr;
Head g_pose;           // pose of the frame being built (taken at the projection)
bool g_immersive = false;
float g_tanX = 0.0f, g_tanY = 0.0f;
int g_log = 0;         // PS2_GT4_CAM_LOG: 1 = changes, 2 = matrices every 300 builds
std::uint64_t g_builds = 0;

// Per-eye rendering test (Wipeout Rewrite VR's method: the 3D world drawn once per eye). The race
// scene renderer 0x1EB0F8(a0, a1, a2, a3, t0, f12) is called from 0x1EA43C and 0x1EA900; its
// arguments come from callee-saved registers there, so right after it returns (probes 0x1EA444 /
// 0x1EA908) it can be called again with the same arguments. Pass 2 adds an eye offset in the
// modelview hook. PS2_GT4_EYE2=x (+ PS2_GT4_EYE2_YAW=deg): second pass with the camera moved x m right and turned
// yaw degrees left (PC test; off by default, the headset build does not set it).
constexpr std::uint32_t kSceneRenderer = 0x1EB0F8u;
constexpr std::uint32_t kAfterScene1 = 0x1EA444u;
constexpr std::uint32_t kAfterScene2 = 0x1EA908u;
bool g_eye2_on = false;
float g_eye2_x = 0.0f, g_eye2_yaw = 0.0f;
int g_pass = 0;              // 0 = the game's own call, 2 = the second (per-eye) pass
bool g_scene_seen = false;   // the camera hooks ran inside the renderer call that just returned
std::uint64_t g_eye2_passes = 0, g_eye2_failed = 0;

// Eye space of GT4's view (OpenGL-like: x right, y up, z back), the same axes as OpenXR. The sign
// per axis is set by PS2_GT4_CAM_SIGNS ("+++" default) until verified in the headset.
float g_sx = 1.0f, g_sy = 1.0f, g_sz = 1.0f;
bool g_use_position = true;

void dump(const char *what, const ps2::Cpu &c)
{
    std::fprintf(stderr, "[gt4cam] %s", what);
    for (int k = 0; k < 4; ++k)
    {
        const float *f = c.vu0.vf[28 + k].f;
        std::fprintf(stderr, " | %.4f %.4f %.4f %.4f", f[0], f[1], f[2], f[3]);
    }
    std::fprintf(stderr, "\n");
}

void after_projection(ps2::Cpu &c)
{
    ++g_builds;
    g_scene_seen = true;
    g_pose = g_head ? g_head() : Head{};
    float *col[4];
    for (int k = 0; k < 4; ++k)
        col[k] = c.vu0.vf[28 + k].f;
    if (g_log >= 2 && g_builds % 300 == 1)
        dump("P", c);
    // Game projection: x_clip = P00 x + P20 z, w_clip = P23 z (GL: -z). tan(half fov) = |P23| / P00
    // for a symmetric frustum (GL: P00 = 1/tan).
    const float w = std::fabs(col[2][3]) > 1e-6f ? std::fabs(col[2][3]) : 1.0f;
    const float gameTanX = std::fabs(col[0][0]) > 1e-6f ? w / std::fabs(col[0][0]) : 0.0f;
    const float gameTanY = std::fabs(col[1][1]) > 1e-6f ? w / std::fabs(col[1][1]) : 0.0f;
    const bool immersive = g_pose.key != 0 && g_pose.tanX > 0.05f && g_pose.tanY > 0.05f && gameTanX > 0.0f &&
                           gameTanY > 0.0f;
    g_tanX = gameTanX;
    g_tanY = gameTanY;
    if (immersive)
    {
        const float kx = gameTanX / g_pose.tanX, ky = gameTanY / g_pose.tanY;
        for (int k = 0; k < 4; ++k)
        {
            col[k][0] *= kx;
            col[k][1] *= ky;
        }
        g_tanX = g_pose.tanX;
        g_tanY = g_pose.tanY;
    }
    if (immersive != g_immersive || (g_log >= 1 && g_builds % 600 == 1))
        std::fprintf(stderr, "[gt4cam] race view %s: game fov tan %.3f x %.3f -> %.3f x %.3f\n",
                     immersive ? "immersive" : "game projection", gameTanX, gameTanY, g_tanX, g_tanY);
    g_immersive = immersive;
}

void after_modelview(ps2::Cpu &c)
{
    if (g_log >= 2 && g_builds % 300 == 1)
        dump("V", c);
    // PS2_GT4_CAM_LOG=3: the camera's world position every 30 builds (units calibration by speed).
    if (g_log == 3 && g_builds % 30 == 1)
    {
        const float *c0 = c.vu0.vf[28].f, *c1 = c.vu0.vf[29].f, *c2 = c.vu0.vf[30].f, *t = c.vu0.vf[31].f;
        // V = [R | t] column-major: position = -R^T t, R rows = (c0[i], c1[i], c2[i]).
        (void)c0; (void)c1; (void)c2;
        float p[3]; // p_j = -sum_i R_ij t_i (R orthonormal), R_ij = column j, row i
        for (int j = 0; j < 3; ++j)
        {
            const float *col = c.vu0.vf[28 + j].f;
            p[j] = -(col[0] * t[0] + col[1] * t[1] + col[2] * t[2]);
        }
        std::fprintf(stderr, "[gt4cam] pos f%llu %.3f %.3f %.3f\n", static_cast<unsigned long long>(ps2::frame_count()), p[0], p[1], p[2]);
    }
    if (g_pass == 2)
    {
        // Second pass: the other eye. V' = R_yaw^T * T(-x) * V in GT4's eye space.
        float *t = c.vu0.vf[31].f;
        t[0] -= g_eye2_x * g_sx;
        if (g_eye2_yaw != 0.0f)
        {
            const float a = g_eye2_yaw * 0.0174533f, ca = std::cos(a), sa = std::sin(a);
            for (int k = 0; k < 4; ++k)
            {
                float *f = c.vu0.vf[28 + k].f;
                const float x0 = f[0], z0 = f[2];
                // R about +y by a (left), R^T applied: x' = ca x - sa z, z' = sa x + ca z
                f[0] = ca * x0 - sa * z0;
                f[2] = sa * x0 + ca * z0;
            }
        }
    }
    if (g_pose.key == 0)
        return;
    // R from the quaternion in GT4's eye space; V' = R^T * V (xyz of every column).
    const float x = g_pose.qx * g_sx, y = g_pose.qy * g_sy, z = g_pose.qz * g_sz, w = g_pose.qw;
    const float r[3][3] = {
        {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
        {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
        {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)},
    };
    // Head position (as Wipeout Pure renders each eye from its real pose): the camera moves with
    // the head, V' = R^T * T(-p) * V, i.e. p off the translation column before the rotation. GT4's
    // units are metres (driving camera near plane 0.1); PS2_GT4_CAM_POS=0 turns it off.
    if (g_use_position)
    {
        float *t = c.vu0.vf[31].f;
        t[0] -= g_pose.px * g_sx;
        t[1] -= g_pose.py * g_sy;
        t[2] -= g_pose.pz * g_sz;
    }
    for (int k = 0; k < 4; ++k)
    {
        float *f = c.vu0.vf[28 + k].f;
        const float c0 = f[0], c1 = f[1], c2 = f[2];
        for (int i = 0; i < 3; ++i)
            f[i] = r[0][i] * c0 + r[1][i] * c1 + r[2][i] * c2;
    }
    if (g_frame)
        g_frame(g_pose.key, g_tanX, g_tanY, g_immersive);
}

// After the game's own renderer call returned: run it once more for the second eye.
void after_scene(ps2::Cpu &c, std::uint32_t return_pc, bool second_caller)
{
    const bool seen = g_scene_seen;
    g_scene_seen = false;
    if (!g_eye2_on || g_pass != 0 || !seen)
        return;
    const ps2::Cpu saved = c;
    // Arguments exactly as the call site sets them (callee-saved registers, unchanged by the call).
    if (!second_caller)
    {   // 0x1EA428..0x1EA440
        c.r[4].d[0] = c.r[17].d[0];   // a0 = s1
        c.r[5].d[0] = c.r[29].d[0];   // a1 = sp
        c.r[6].d[0] = c.r[20].d[0];   // a2 = s4
        c.r[7].d[0] = c.r[22].d[0];   // a3 = s6
        c.r[8].sd[0] = -1;            // t0 = -1
        c.f[12] = c.f[21];            // f12 = f21
    }
    else
    {   // 0x1EA8EC..0x1EA904 (t0 = s5, a1 = s2, a2 = s3, a3 = sp, a0 = s0, f12 = f20)
        c.r[4].d[0] = c.r[16].d[0];
        c.r[5].d[0] = c.r[18].d[0];
        c.r[6].d[0] = c.r[19].d[0];
        c.r[7].d[0] = c.r[29].d[0];
        c.r[8].d[0] = c.r[21].d[0];
        c.f[12] = c.f[20];
    }
    c.r[31].sd[0] = static_cast<std::int32_t>(return_pc);
    g_pass = 2;
    ps2::guest_call_hold_begin(c);
    ps2::call(c, kSceneRenderer, 1u);
    ps2::guest_call_hold_end(c);
    g_pass = 0;
    g_scene_seen = false;
    const std::uint32_t end_pc = c.pc;
    const bool ok = end_pc == return_pc;
    const std::int32_t budget = c.budget;
    c = saved;
    c.budget = budget;
    if (ok)
        ++g_eye2_passes;
    else
    {
        ++g_eye2_failed;
        std::fprintf(stderr, "[gt4cam] eye 2 pass did not return (pc 0x%08X, want 0x%08X): per-eye pass off\n",
                     end_pc, return_pc);
        g_eye2_on = false;
    }
    if (g_log >= 1 && (g_eye2_passes % 600) == 1)
        std::fprintf(stderr, "[gt4cam] eye 2 passes %llu (failed %llu)\n",
                     static_cast<unsigned long long>(g_eye2_passes), static_cast<unsigned long long>(g_eye2_failed));
}

// ps2run test pose: PS2_GT4_TEST_YAW / _PITCH (degrees, positive = left / up), PS2_GT4_TEST_TAN
// ("tx,ty" wanted fov).
Head test_head()
{
    Head h;
    const char *yaw = std::getenv("PS2_GT4_TEST_YAW");
    const char *pitch = std::getenv("PS2_GT4_TEST_PITCH");
    // PS2_GT4_TEST_FROM=frame: the yaw starts at that frame (pipeline delay measurement).
    static const char *from = std::getenv("PS2_GT4_TEST_FROM");
    const bool started = !from || ps2::frame_count() >= std::strtoull(from, nullptr, 10);
    static bool logged = false;
    if (from && started && !logged)
    {
        logged = true;
        std::fprintf(stderr, "[gt4cam] test yaw starts in the camera built at frame %llu\n",
                     static_cast<unsigned long long>(ps2::frame_count()));
    }
    const float ay = (yaw && started ? std::strtof(yaw, nullptr) : 0.0f) * 0.0174533f * 0.5f;
    const float ap = (pitch ? std::strtof(pitch, nullptr) : 0.0f) * 0.0174533f * 0.5f;
    // q = yaw about +y, then pitch about +x (OpenXR axes)
    const float cy = std::cos(ay), sy = std::sin(ay), cp = std::cos(ap), sp = std::sin(ap);
    h.qw = cy * cp;
    h.qx = cy * sp;
    h.qy = sy * cp;
    h.qz = -sy * sp;
    h.key = 1;
    if (const char *t = std::getenv("PS2_GT4_TEST_TAN"))
        std::sscanf(t, "%f,%f", &h.tanX, &h.tanY);
    return h;
}
} // namespace

void install(HeadSource head, FrameSink frame)
{
    if (const char *l = std::getenv("PS2_GT4_CAM_LOG"))
        g_log = std::atoi(l);
    if (const char *s = std::getenv("PS2_GT4_CAM_SIGNS"); s && std::strlen(s) >= 3)
    {
        g_sx = s[0] == '-' ? -1.0f : 1.0f;
        g_sy = s[1] == '-' ? -1.0f : 1.0f;
        g_sz = s[2] == '-' ? -1.0f : 1.0f;
    }
    if (const char *p = std::getenv("PS2_GT4_CAM_POS"))
        g_use_position = std::atoi(p) != 0;
    g_head = head;
    if (!g_head && (std::getenv("PS2_GT4_TEST_YAW") || std::getenv("PS2_GT4_TEST_PITCH") || std::getenv("PS2_GT4_TEST_TAN")))
        g_head = test_head;
    g_frame = frame;
    ps2::set_probe_hook(kAfterProjection, after_projection);
    ps2::set_probe_hook(kAfterModelview, after_modelview);
    if (const char *e = std::getenv("PS2_GT4_EYE2"); e && e[0] != '\0')
    {
        g_eye2_x = std::strtof(e, nullptr);
        if (const char *y = std::getenv("PS2_GT4_EYE2_YAW"))
            g_eye2_yaw = std::strtof(y, nullptr);
        g_eye2_on = true;
        std::fprintf(stderr, "[gt4cam] per-eye test: second race pass, camera %.3f m right, %.1f deg left\n",
                     g_eye2_x, g_eye2_yaw);
    }
    ps2::set_probe_hook(kAfterScene1, [](ps2::Cpu &c) { after_scene(c, kAfterScene1, false); });
    ps2::set_probe_hook(kAfterScene2, [](ps2::Cpu &c) { after_scene(c, kAfterScene2, true); });
}
} // namespace gt4::camera

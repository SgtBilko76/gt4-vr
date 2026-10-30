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

// Eye space of GT4's view (OpenGL-like: x right, y up, z back), the same axes as OpenXR. The sign
// per axis is set by PS2_GT4_CAM_SIGNS ("+++" default) until verified in the headset.
float g_sx = 1.0f, g_sy = 1.0f, g_sz = 1.0f;

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
    if (g_pose.key == 0)
        return;
    // R from the quaternion in GT4's eye space; V' = R^T * V (xyz of every column).
    const float x = g_pose.qx * g_sx, y = g_pose.qy * g_sy, z = g_pose.qz * g_sz, w = g_pose.qw;
    const float r[3][3] = {
        {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
        {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
        {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)},
    };
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

// ps2run test pose: PS2_GT4_TEST_YAW / _PITCH (degrees, positive = left / up), PS2_GT4_TEST_TAN
// ("tx,ty" wanted fov).
Head test_head()
{
    Head h;
    const char *yaw = std::getenv("PS2_GT4_TEST_YAW");
    const char *pitch = std::getenv("PS2_GT4_TEST_PITCH");
    const float ay = (yaw ? std::strtof(yaw, nullptr) : 0.0f) * 0.0174533f * 0.5f;
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
    g_head = head;
    if (!g_head && (std::getenv("PS2_GT4_TEST_YAW") || std::getenv("PS2_GT4_TEST_PITCH") || std::getenv("PS2_GT4_TEST_TAN")))
        g_head = test_head;
    g_frame = frame;
    ps2::set_probe_hook(kAfterProjection, after_projection);
    ps2::set_probe_hook(kAfterModelview, after_modelview);
}
} // namespace gt4::camera

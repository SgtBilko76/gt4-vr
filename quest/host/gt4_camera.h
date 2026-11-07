// SPDX-License-Identifier: GPL-3.0-or-later
//
// GT4 (A-Spec II) race camera hooks for VR: turn the game's race view by the head rotation and
// widen its projection to the headset's field of view. Platform neutral (the Quest host feeds the
// head pose; ps2run can feed a fixed test pose through PS2_GT4_TEST_* variables).
#pragma once

#include <cstdint>

namespace gt4::camera
{
struct Head
{
    float qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 1.0f; // OpenXR head rotation (x right, y up, z back)
    float px = 0.0f, py = 0.0f, pz = 0.0f;            // head position from the centred one (metres)
    int64_t key = 0;                                  // pose time; 0 = not tracked (game camera)
    float tanX = 0.0f, tanY = 0.0f;                   // wanted tan(half fov); 0 = game projection
    float shiftY = 0.0f; // window moved down by this tan (off-axis): the frame covers tan -(tanY+shiftY)..tanY-shiftY
};
using HeadSource = Head (*)();
// The race view was built: with the pose `key` and the projection's tan(half fov).
using FrameSink = void (*)(int64_t key, float tanX, float tanY, bool immersive);

// Registers the probe hooks (the addresses must be in games/<game>/probes.txt). retail = the
// retail USA v2.00 program (GT4 Online VR) instead of A-Spec II.
void install(HeadSource head, FrameSink frame, bool retail = false);
// Race scene detection: the counter grows with every race camera build (races and replays, not
// menus). installed() is false when the hooks are not in this build (retail GT4 Online).
bool installed();
// Retail GT4 (no camera hooks): only count the race scene builds, so the XR host can tell races from
// menus / result screens (3D on in races only). Probe at the retail counterpart of 0x1EB1F4.
void installSceneDetector();
// Retail: the race renderer's state at its last scene build - mode (+0x1B8: 0 driving, 3 briefing,
// 4 test menu, 6 result), showcase (+0xBC: 0 driving, 1.. after the finish) and the picture-in-
// picture flag. The XR host decides from these whether the depth 3D is on.
struct SceneState {
    std::uint32_t mode = 0, showcase = 0;
    bool windowed = false;
};
SceneState sceneState();
std::uint64_t sceneBuilds();
} // namespace gt4::camera

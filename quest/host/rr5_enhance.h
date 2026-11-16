// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ridge Racer V (SLUS-20002) enhancements on the runtime's probe hooks (see rr5_enhance.cpp).
#pragma once

#include "gt4_camera.h"

#include <cstdint>

namespace rr5::enhance
{
// RRV_RR5_WIDESCREEN=16:9|16:10|21:9: the game's camera renders a wider view. Returns true when on.
bool installWidescreen();
// VR race view: in a race (driving phases) the game camera is turned by the head (and, with the pose's
// tanX / tanY, renders the headset's field of view). head = nullptr: PS2_RR5_TEST_* pose (ps2run).
void installRaceView(gt4::camera::HeadSource head, gt4::camera::FrameSink frame);
// Grows with every driving-view camera build of a race (the XR host's "racing" signal).
std::uint64_t raceBuilds();
} // namespace rr5::enhance

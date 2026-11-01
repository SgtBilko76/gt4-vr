// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host-driven guest calls that must run to completion (GT4 VR: the race renderer called a second
// time for the other eye, quest/host/gt4_camera.cpp). Between begin and end the EE is not
// preempted (no interrupt delivery, no thread switch: the generated code's budget checks keep
// going), and end puts the guest clock back where begin found it, so the extra call costs the game
// no guest time. Interrupts that fell due meanwhile are delivered at the next budget check.
#pragma once

#include "ps2/cpu.hpp"

namespace ps2 {
void guest_call_hold_begin(Cpu &c);
void guest_call_hold_end(Cpu &c);
}

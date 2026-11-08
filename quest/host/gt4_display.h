// GT4: start in 480p progressive (owner 2026-11-08: the VR picture is far sharper in 480p, but GT4 never
// applies the saved display setting at start and falls back after videos). See gt4_display.cpp.
#pragma once

namespace gt4::display {
// retail = GT4 Online v2.00 (SCUS-97328), else A-Spec II. PS2_GT4_480P: 1 while racing (default),
// 2 always, 0 off. Needs the race camera hooks (gt4::camera) for the race test.
void install(bool retail);
// Call once per vblank (emulation thread, between guest frames).
void tick();
} // namespace gt4::display

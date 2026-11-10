// SPDX-License-Identifier: GPL-3.0-or-later
// Builds without GT4 Online (the A-Spec II app, GT4VR_ONLINE OFF): no leaderboard reports or race
// rooms are compiled in. The VR menu (online_panel.cpp) runs with its Settings page only and calls
// these instead of gt4_results.cpp.
#include "gt4_results.h"

namespace gt4::results {
Room room() { return {}; }
void createRoom() {}
void joinRoom(const std::string &) {}
void leaveRoom() {}
int pingMs() { return -1; }
void notePing(int) {}
}  // namespace gt4::results

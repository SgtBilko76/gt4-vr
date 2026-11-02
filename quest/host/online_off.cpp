// Builds without GT4 Online's online menu (the A-Spec II app, GT4VR_ONLINE OFF): the panel API the XR
// host calls, with no menu behind it. No leaderboard, lap reports or race rooms are compiled in.
#include "online_panel.h"

namespace gt4::panel {
bool start(const std::string &, const std::string &, ContextFn) { return false; }
bool available() { return false; }
void setOpen(bool) {}
bool isOpen() { return false; }
void toggle() {}
void input(int, int) {}
void setTab(int) {}
void select() {}
bool render(std::vector<std::uint8_t> &) { return false; }
}  // namespace gt4::panel

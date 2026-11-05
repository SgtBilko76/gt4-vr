#pragma once

// GT4 Online's VR online menu: a panel in front of the curved screen with the leaderboard data of
// gt4-results (gt4.portroyale.online). Platform-neutral: it fetches the data on its own thread and
// draws the panel into an RGBA image; quest_xr.cpp shows that image as a quad layer and feeds the
// controller input. Owner 2026-11-02: leaderboard + running races inside the game.
#include <cstdint>
#include <string>
#include <vector>

namespace gt4::panel {

constexpr int kWidth = 1280;
constexpr int kHeight = 800;

// What the game knows right now (gt4_results.cpp): highlights the player and picks the Track tab's
// track. Empty fields = unknown (menus).
struct Context {
    std::string player, track, car;
};
using ContextFn = Context (*)();

// server = "host[:port]" of gt4-results, font = path of a TrueType font. Starts the fetch thread.
// Returns false (panel unavailable) when the font can't be loaded.
bool start(const std::string &server, const std::string &font, ContextFn context);
bool available();

void setOpen(bool open);
bool isOpen();
void toggle();

// Controller input while open: dx = -1/+1 previous/next tab, dy = -1/+1 scroll up/down.
void input(int dx, int dy);
void setTab(int tab);  // 0 Races, 1 Live, 2 Leaderboard, 3 Track, 4 Recent
// A: on the Races tab, create / leave a race or join the selected one.
void select();

// Draws the panel into rgba (kWidth * kHeight, R G B A bytes, top row first) when something
// changed since the last call (data, tab, scroll, open). Returns false when nothing changed.
bool render(std::vector<std::uint8_t> &rgba);
// The "Online" badge shown while the relay link is up: a dark pill with a green dot and the word,
// kBadgeWidth * kBadgeHeight RGBA (transparent around it, top row first). False without the font.
constexpr int kBadgeWidth = 320;
constexpr int kBadgeHeight = 96;
bool renderBadge(std::vector<std::uint8_t> &rgba);

}

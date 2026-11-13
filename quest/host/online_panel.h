#pragma once

// The VR menu: a panel in front of the curved screen. Pages: Lobby (GT4 Online only: open race rooms
// from gt4-results' /api/rooms and how to connect) and Settings (screen distance / size, swapped
// triggers / grips: gt4_settings.h). Platform-neutral: it fetches the lobby data on its own thread and
// draws the panel into an RGBA image; quest_xr.cpp shows that image as a quad layer and feeds the
// controller input. Owner 2026-11-10: lobby without leaderboard; settings in both apps.
#include <cstdint>
#include <string>
#include <vector>

namespace gt4::panel {

constexpr int kWidth = 1280;
constexpr int kHeight = 800;

// What the game knows right now (gt4_results.cpp): the player's name for the header. Empty fields =
// unknown (menus).
struct Context {
    std::string player, track, car;
};
using ContextFn = Context (*)();

// server = "host[:port]" of gt4-results ("" = no lobby page: Spec II, or reports switched off),
// font = path of a TrueType font. Starts the fetch thread. Returns false (panel unavailable) when the
// font can't be loaded.
bool start(const std::string &server, const std::string &font, ContextFn context);
bool available();

// Start-up notice (0.3.2: users saw a black screen when file access or the ISO was missing): while
// set, the panel shows `heading` and `body` (\n = new paragraph, long lines wrap) under the app name and
// counts as open; the player cannot close it and the menu input is ignored. Works before start().
// Returns false when the font can't be loaded. clearNotice() removes it.
bool setNotice(const std::string &font, const std::string &app, const std::string &heading, const std::string &body);
void clearNotice();

void setOpen(bool open);
bool isOpen();
void toggle();

// Controller input while open: dy = -1/+1 choose the row above/below; dx = -1/+1 changes the
// selected setting on the Settings page.
void input(int dx, int dy);
void nextPage(int d);  // grips: Lobby <-> Settings (GT4 Online)
void setTab(int tab);  // 0 Lobby (if there is one), else Settings
// A: lobby - create / leave a race or join the selected one; settings - toggle / reset.
void select();

// Draws the panel into rgba (kWidth * kHeight, R G B A bytes, top row first) when something
// changed since the last call (data, selection, open). Returns false when nothing changed.
bool render(std::vector<std::uint8_t> &rgba);
// The "Online" badge shown while the relay link is up: a dark pill with a green dot and the word,
// kBadgeWidth * kBadgeHeight RGBA (transparent around it, top row first). False without the font.
constexpr int kBadgeWidth = 320;
constexpr int kBadgeHeight = 96;
bool renderBadge(std::vector<std::uint8_t> &rgba);

}

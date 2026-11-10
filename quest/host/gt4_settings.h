#pragma once

// Player settings from the VR menu's Settings page (owner 2026-11-10): screen distance and size of the
// curved screen, swapped triggers / grips. Stored in <data dir>/settings.txt (KEY=VALUE lines), read
// at start, written on every change. Thread-safe: the XR thread reads, the menu writes.
#include <string>

namespace gt4::settings {

struct Values {
    float screen_distance_m = 3.0f;  // curved / flat screen distance
    int screen_size_pct = 100;       // screen width in % of the default width
    bool swap_triggers = false;      // false: right trigger throttle, left trigger brake
    bool swap_grips = false;         // false: right grip shift up, left grip shift down
};

constexpr float kMinDistanceM = 1.5f, kMaxDistanceM = 8.0f, kDistanceStepM = 0.25f;
constexpr int kMinSizePct = 50, kMaxSizePct = 200, kSizeStepPct = 5;

void load(const std::string &path);  // also remembers the path for save()
Values get();
void set(const Values &v);           // clamps, stores, writes the file
unsigned version();                  // bumped by every set(): readers re-apply when it changes

}  // namespace gt4::settings

// SPDX-License-Identifier: GPL-3.0-or-later
// Player settings, see gt4_settings.h.
#include "gt4_settings.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>

namespace gt4::settings {

namespace {

std::mutex g_mutex;
Values g_values;
std::string g_path;
std::atomic<unsigned> g_version{1};

Values clamp(Values v) {
    v.screen_distance_m = std::clamp(v.screen_distance_m, kMinDistanceM, kMaxDistanceM);
    v.screen_distance_m = std::round(v.screen_distance_m / kDistanceStepM) * kDistanceStepM;
    v.screen_size_pct = std::clamp(v.screen_size_pct, kMinSizePct, kMaxSizePct);
    v.stereo_pct = std::clamp(v.stereo_pct, kMinStereoPct, kMaxStereoPct);
    return v;
}

void save_locked() {
    if (g_path.empty()) return;
    const std::string tmp = g_path + ".tmp";
    if (std::FILE *f = std::fopen(tmp.c_str(), "w")) {
        std::fprintf(f, "# GT4 VR settings (VR menu > Settings). Delete this file for the defaults.\n");
        std::fprintf(f, "SCREEN_DISTANCE_M=%.2f\n", g_values.screen_distance_m);
        std::fprintf(f, "SCREEN_SIZE_PCT=%d\n", g_values.screen_size_pct);
        std::fprintf(f, "STEREO_PCT=%d\n", g_values.stereo_pct);
        std::fprintf(f, "SWAP_TRIGGERS=%d\n", g_values.swap_triggers ? 1 : 0);
        std::fprintf(f, "SWAP_GRIPS=%d\n", g_values.swap_grips ? 1 : 0);
        std::fclose(f);
        std::rename(tmp.c_str(), g_path.c_str());
    }
}

}  // namespace

void load(const std::string &path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_path = path;
    Values v;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "SCREEN_DISTANCE_M") v.screen_distance_m = std::strtof(value.c_str(), nullptr);
        else if (key == "SCREEN_SIZE_PCT") v.screen_size_pct = std::atoi(value.c_str());
        else if (key == "STEREO_PCT") v.stereo_pct = std::atoi(value.c_str());
        else if (key == "SWAP_TRIGGERS") v.swap_triggers = std::atoi(value.c_str()) != 0;
        else if (key == "SWAP_GRIPS") v.swap_grips = std::atoi(value.c_str()) != 0;
    }
    g_values = clamp(v);
    g_version.fetch_add(1);
}

Values get() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_values;
}

void set(const Values &v) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_values = clamp(v);
    save_locked();
    g_version.fetch_add(1);
}

unsigned version() { return g_version.load(); }

}  // namespace gt4::settings

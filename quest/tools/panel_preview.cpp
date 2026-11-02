// PC preview of GT4 Online's VR online menu (quest/host/online_panel.cpp) with live server data.
// build: g++ -std=c++17 -O1 -pthread -o panel_preview quest/tools/panel_preview.cpp quest/host/online_panel.cpp
// usage: panel_preview FONT OUT_PREFIX [SERVER] [PLAYER] [TRACK]   -> OUT_PREFIX-<tab>.ppm for all 4 tabs
#include "../host/online_panel.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

static std::string g_player, g_track;
static gt4::panel::Context context() { return {g_player, g_track, ""}; }

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s FONT OUT_PREFIX [SERVER] [PLAYER] [TRACK]\n", argv[0]);
        return 2;
    }
    g_player = argc > 4 ? argv[4] : "";
    g_track = argc > 5 ? argv[5] : "";
    if (!gt4::panel::start(argc > 3 ? argv[3] : "", argv[1], context)) return 1;
    gt4::panel::setOpen(true);
    std::this_thread::sleep_for(std::chrono::seconds(4));
    std::vector<std::uint8_t> rgba;
    for (int tab = 0; tab < 4; ++tab) {
        if (tab) gt4::panel::input(1, 0);
        gt4::panel::render(rgba);
        const std::string path = std::string(argv[2]) + "-" + std::to_string(tab) + ".ppm";
        FILE *f = std::fopen(path.c_str(), "wb");
        std::fprintf(f, "P6\n%d %d\n255\n", gt4::panel::kWidth, gt4::panel::kHeight);
        for (std::size_t i = 0; i < rgba.size(); i += 4) std::fwrite(&rgba[i], 1, 3, f);
        std::fclose(f);
        std::printf("%s\n", path.c_str());
    }
    return 0;
}

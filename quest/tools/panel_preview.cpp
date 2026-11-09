// PC preview of GT4 Online's VR online menu (quest/host/online_panel.cpp) with live server data.
// build: g++ -std=c++17 -O1 -pthread -o panel_preview quest/tools/panel_preview.cpp quest/host/online_panel.cpp
// usage: panel_preview FONT OUT_PREFIX [SERVER] [PLAYER] [TRACK] [create]  -> OUT_PREFIX-<tab>.ppm for all 5 tabs
#include "../host/gt4_results.h"
#include "../host/online_panel.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <unistd.h>

static std::string g_player, g_track;
static gt4::panel::Context context() { return {g_player, g_track, ""}; }

// Room state without the network link (the preview has no relay connection).
static gt4::results::Room g_room;
namespace gt4::results {
Room room() { return g_room; }
void createRoom() { g_room = {"Rpreview", true}; }
void joinRoom(const std::string &id) { g_room = {id, false}; }
void leaveRoom() { g_room = {}; }
// Ping as in gt4_results.cpp: median of the last 5 handshakes to the server.
static std::vector<int> g_ping;
int pingMs() {
    if (g_ping.empty()) return -1;
    std::vector<int> s = g_ping;
    std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end());
    return s[s.size() / 2];
}
void notePing(int ms) {
    g_ping.push_back(ms);
    if (g_ping.size() > 5) g_ping.erase(g_ping.begin());
}
}

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s FONT OUT_PREFIX [SERVER] [PLAYER] [TRACK]\n", argv[0]);
        return 2;
    }
    g_player = argc > 4 ? argv[4] : "";
    g_track = argc > 5 ? argv[5] : "";
    if (!gt4::panel::start(argc > 3 ? argv[3] : "", argv[1], context)) return 1;
    if (argc > 6) {
        const std::string room = argv[6];
        if (room == "create") gt4::results::createRoom();
        else gt4::results::joinRoom(room);
    }
    gt4::panel::setOpen(true);
    std::this_thread::sleep_for(std::chrono::seconds(4));
    std::vector<std::uint8_t> rgba;
    for (int tab = 0; tab < 5; ++tab) {
        if (tab) gt4::panel::input(1, 0);
        gt4::panel::render(rgba);
        const std::string path = std::string(argv[2]) + "-" + std::to_string(tab) + ".ppm";
        FILE *f = std::fopen(path.c_str(), "wb");
        std::fprintf(f, "P6\n%d %d\n255\n", gt4::panel::kWidth, gt4::panel::kHeight);
        for (std::size_t i = 0; i < rgba.size(); i += 4) std::fwrite(&rgba[i], 1, 3, f);
        std::fclose(f);
        std::printf("%s\n", path.c_str());
    }
    // The panel's fetch thread keeps running: leave without destroying the globals it uses.
    std::fflush(stdout);
    _exit(0);
}

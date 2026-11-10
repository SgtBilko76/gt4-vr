// Renders the GT4 Online menu (online_panel.cpp) to PPM files on the PC, with stubbed room state and
// race-list data from a running gt4-results (or none), to check the layout without the headset.
//   g++ -std=c++20 -O1 -Iquest/host tests/online_panel_preview.cpp quest/host/online_panel.cpp -o panel_preview
//   ./panel_preview <out dir> [server host:port]
#include "gt4_results.h"
#include "online_panel.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {
gt4::results::Room g_room;
}

namespace gt4::results {
Room room() { return g_room; }
void createRoom() { g_room = {"R123456", true}; }
void joinRoom(const std::string &id) { g_room = {id, false}; }
void leaveRoom() { g_room = {}; }
int pingMs() { return 23; }
void notePing(int) {}
}

static void save(const std::string &path, const std::vector<std::uint8_t> &rgba) {
    std::FILE *f = std::fopen(path.c_str(), "wb");
    std::fprintf(f, "P6\n%d %d\n255\n", gt4::panel::kWidth, gt4::panel::kHeight);
    for (std::size_t i = 0; i < rgba.size(); i += 4) std::fwrite(&rgba[i], 1, 3, f);
    std::fclose(f);
}

int main(int argc, char **argv) {
    const std::string out = argc > 1 ? argv[1] : ".";
    const std::string server = argc > 2 ? argv[2] : "127.0.0.1:1";
    if (!gt4::panel::start(server, "quest/android/build/stage/assets/gt4vr/resources/fonts/Roboto-Regular.ttf",
                           [] { return gt4::panel::Context{"Bilko", "", ""}; }))
        return 1;
    gt4::panel::setOpen(true);
    std::this_thread::sleep_for(std::chrono::seconds(3));
    std::vector<std::uint8_t> px;
    const struct {
        const char *name;
        gt4::results::Room room;
    } states[] = {{"lobby", {}}, {"host", {"R123456", true}}, {"joined", {"R654321", false}}};
    for (const auto &s : states) {
        g_room = s.room;
        gt4::panel::input(0, 1);
        gt4::panel::input(0, -1);
        gt4::panel::render(px);
        save(out + "/panel_" + s.name + ".ppm", px);
    }
    std::_Exit(0);
}

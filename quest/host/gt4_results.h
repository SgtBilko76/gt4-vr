#pragma once

// GT4 Online (retail SCUS-97328) lap results for the leaderboard: probes on the race conductor's
// line-crossing functions read lap times, and a background thread sends them to the gt4-results
// service (gt4.portroyale.online).
//
// Environment (the Quest app sets defaults, config.txt overrides):
//   PS2_RESULTS_SERVER  host[:port] of gt4-results (port 27314); unset or "off" = nothing is sent
//   PS2_RESULTS_LOG     file that gets one line per own lap (kept on the headset)
//   PS2_NET_NAME        device id sent with each report (Quest-xxxxxx)
//   PS2_PLAYER_NAME     player name if the GT4 save name can't be read
//   PS2_GT4_LAPLOG=1    log every line crossing of every car
//   PS2_GT4_RESULTS_CAR=n  treat car n as the player's (PC tests with an AI car)
//   PS2_ROOM=create|<id>   start in a race room (tests; normally chosen in the online menu)
#include <string>

namespace gt4::results {

// Installs the probe hooks. The probe addresses must be compiled in (games/gt4r/probes.txt).
void install();

// What the game has set up right now (read from its race setup; empty in menus without a race):
// the GT4 save name, the track and car display names.
void current(std::string &player, std::string &track, std::string &car);

// Race rooms (GT4 Online's online menu): a room is its own relay group, so only its players find
// each other in Multi-LAN Race. While in a room, a status heartbeat every 10 s lists it on the
// server (api/rooms); the host's room id is "R" + the headset's network id.
struct Room {
    std::string id;   // "" = no room (the shared group everyone is in)
    bool host = false;
};
Room room();
void createRoom();
void joinRoom(const std::string &id);
void leaveRoom();

}

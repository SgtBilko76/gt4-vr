#pragma once

// GT4 Online (retail SCUS-97328) race rooms and lap times: probes on the race conductor's
// line-crossing functions read lap times into laps.txt on the headset (not sent anywhere since
// 2026-11-10: no leaderboard). While in a race room, a status heartbeat goes to the gt4-results
// service so the room shows up in the online menu's lobby browser.
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
// Round trip time to the gt4-results server in ms (-1 = not measured yet). The relay runs on the same
// host, so it is also the player's ping to the relay. Measured from TCP handshakes (one round trip
// each) of the status reports and the online panel's requests: median of the last 5.
int pingMs();
void notePing(int ms);
void joinRoom(const std::string &id);
void leaveRoom();

// GT4's LAN Settings (GT Mode > Options > LAN Settings), written straight into the game's object so
// the player never visits that menu: host = this console creates the race, players = how many
// consoles the host waits for (1..6). False when the object isn't in memory (game not booted).
// Test env (ps2run / config.txt): PS2_LAN_HOST=0|1 + PS2_LAN_PLAYERS=n are applied once the object
// exists.
bool setLanSettings(bool host, int players);
bool lanSettings(bool &host, int &players);

// Drives GT4's menus to the Multi-LAN lobby on its own (from GT Mode's map or Home, the title
// screen or the Arcade menu): sets Host/players, then presses the buttons screen by screen,
// checking which screen is open before every step. createRoom()/joinRoom() start it; the status
// line is for the online panel ("" when idle). Test: PS2_LAN_AUTO=host|join.
void startLobbyDrive(bool host, int players = 2);
void stopLobbyDrive();
std::string lobbyDriveStatus();

}

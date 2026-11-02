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
namespace gt4::results {

// Installs the probe hooks. The probe addresses must be compiled in (games/gt4r/probes.txt).
void install();

}

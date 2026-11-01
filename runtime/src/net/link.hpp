#pragma once

// The network link behind the emulated PS2 network adapter: Ethernet frames travel through a
// PSP-style ad hoc server (adhocctl TCP 27312 + relay TCP 27313, e.g. wipeout-adhoc-server). The
// server is only a relay: it forwards each frame to the players of the same group (broadcast) or to
// the one with the destination MAC. Every PS2 in a group sees the others as if on one LAN cable.
//
// Configuration (environment, read once):
//   PS2_NET_SERVER  host[:adhocctl port]  enables the link; unset = no network adapter
//   PS2_NET_GROUP   group name, up to 8 characters (default GT4LAN)
//   PS2_NET_NAME    nickname shown by the server (default the host name)
//   PS2_NET_MAC     aa:bb:cc:dd:ee:ff, default derived from PS2_NET_NAME / host name
//   PS2_NET_PRODUCT product code, 9 characters A-Z/0-9 (default SCUS97436)
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ps2::net {

using Mac = std::array<std::uint8_t, 6>;

bool enabled();
Mac mac();

// Starts the background connection once (idempotent).
void start();

// One Ethernet frame (no FCS) out to the group.
void send(const std::uint8_t *frame, std::size_t size);

// Next received frame for this PS2, if any.
bool receive(std::vector<std::uint8_t> &frame);

}

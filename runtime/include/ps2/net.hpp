#pragma once

// The emulated network adapter's link to the ad hoc relay (runtime/src/net/link.cpp).
#include <cstdint>
#include <string>

namespace ps2::net {

// The relay group (8 characters at most): only adapters in the same group see each other. Default
// PS2_NET_GROUP or "GT4LAN"; set_group("") goes back to it. A change reconnects to the relay.
void set_group(const std::string &group);
std::string group();
std::string default_group();
// True while the relay link (PS2_NET_SERVER) is logged in; for a status display.
bool connected();
// DHCP requests the game has sent so far: GT4 asks for an address when Multi-LAN Race connects.
std::uint64_t dhcp_requests();

}

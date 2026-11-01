#pragma once

// A minimal DHCP server inside the emulated network adapter: there is no router behind the ad hoc
// relay, so a PS2 whose network configuration says "automatic" would never get an address. Each PS2
// answers its own DISCOVER/REQUEST locally with an address derived from its MAC:
// 10.76.x.y / 255.255.0.0 (x.y from the MAC's last 16 bits, never .0.0/.0.1/.255.255), router and
// DNS 10.76.0.1 (not present; LAN play needs neither).
#include <cstddef>
#include <cstdint>
#include <vector>

#include "link.hpp"

namespace ps2::net::dhcp {

// If `frame` is a DHCP client message, builds the server's reply into `reply` and returns true
// (the frame must then not go out to the group).
bool handle(const std::uint8_t *frame, std::size_t size, const Mac &mac, std::vector<std::uint8_t> &reply);

// The address handed out for this MAC, host byte order.
std::uint32_t address_for(const Mac &mac);

}

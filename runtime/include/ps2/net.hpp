#pragma once

// The emulated network adapter's link to the ad hoc relay (runtime/src/net/link.cpp).
#include <string>

namespace ps2::net {

// The relay group (8 characters at most): only adapters in the same group see each other. Default
// PS2_NET_GROUP or "GT4LAN"; set_group("") goes back to it. A change reconnects to the relay.
void set_group(const std::string &group);
std::string group();
std::string default_group();

}

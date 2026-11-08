// SPDX-License-Identifier: GPL-3.0-or-later
#include "link.hpp"

#include "ps2/net.hpp"

#include "dhcp.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#ifndef __EMSCRIPTEN__
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#endif

namespace ps2::net {

namespace {

// Wire formats of the PSP ad hoc server (PortableKit host/adhoc/protocol.hpp), little-endian.
constexpr std::uint8_t kCtlPing = 0, kCtlLogin = 1, kCtlConnect = 2;
constexpr std::size_t kNicknameLength = 128, kGroupLength = 8, kProductLength = 9;
constexpr std::size_t kInitSize = 24, kPdpHeader = 14;
// The relay port of the datagram socket that carries the frames ('GT').
constexpr std::uint16_t kFramePort = 0x4754;
constexpr std::size_t kRxQueueMax = 512;

struct Server {
    std::string host;
    std::string ctl_port = "27312", relay_port = "27313";
};

struct Config {
    bool enabled = false;
    // PS2_NET_SERVER=main[:port][,backup[:port]...]: the first is used whenever it answers; the others only
    // when it can't be reached (owner 2026-11-08: backup relay, no load balancing - players on different
    // relays can't see each other). Every reconnect starts with the first again.
    std::vector<Server> servers;
    std::string host;
    std::string ctl_port = "27312", relay_port = "27313";
    std::string group = "GT4LAN", name, product = "SCUS97436";
    Mac mac{};
};

const Config &config() {
    static const Config c = [] {
        Config c;
        const char *server = std::getenv("PS2_NET_SERVER");
        if (!server || !*server || std::strcmp(server, "off") == 0) return c;
        c.enabled = true;
        std::string list = server;
        for (std::size_t at = 0; at <= list.size();) {
            const std::size_t comma = std::min(list.find(',', at), list.size());
            Server s;
            s.host = list.substr(at, comma - at);
            at = comma + 1;
            while (!s.host.empty() && s.host.front() == ' ') s.host.erase(0, 1);
            while (!s.host.empty() && s.host.back() == ' ') s.host.pop_back();
            if (s.host.empty()) continue;
            if (const auto colon = s.host.rfind(':'); colon != std::string::npos) {
                const int port = std::atoi(s.host.c_str() + colon + 1);
                s.host.resize(colon);
                if (port > 0 && port < 65535) {
                    s.ctl_port = std::to_string(port);
                    s.relay_port = std::to_string(port + 1);
                }
            }
            c.servers.push_back(s);
        }
        if (c.servers.empty()) {
            c.enabled = false;
            return c;
        }
        c.host = c.servers.front().host;
        c.ctl_port = c.servers.front().ctl_port;
        c.relay_port = c.servers.front().relay_port;
        if (const char *g = std::getenv("PS2_NET_GROUP"); g && *g) c.group = std::string(g).substr(0, kGroupLength);
        if (const char *p = std::getenv("PS2_NET_PRODUCT"); p && std::strlen(p) == kProductLength) c.product = p;
        if (const char *n = std::getenv("PS2_NET_NAME"); n && *n) c.name = n;
        if (c.name.empty()) {
            char host[256] = {};
            if (gethostname(host, sizeof host - 1) == 0 && host[0]) c.name = host;
            else c.name = "PS2";
        }
        unsigned m[6];
        const char *mac = std::getenv("PS2_NET_MAC");
        if (mac && std::sscanf(mac, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6) {
            for (int i = 0; i < 6; ++i) c.mac[i] = static_cast<std::uint8_t>(m[i]);
        } else {
            // Sony's OUI (as PCSX2's default MAC) + 24 bits of FNV-1a of the nickname.
            std::uint32_t h = 2166136261u;
            for (const char ch : c.name) h = (h ^ static_cast<std::uint8_t>(ch)) * 16777619u;
            c.mac = {0x00, 0x04, 0x1F, static_cast<std::uint8_t>(h >> 16), static_cast<std::uint8_t>(h >> 8),
                     static_cast<std::uint8_t>(h)};
        }
        return c;
    }();
    return c;
}

void put16(std::string &o, std::uint16_t v) {
    o.push_back(static_cast<char>(v));
    o.push_back(static_cast<char>(v >> 8));
}
void put32(std::string &o, std::uint32_t v) {
    put16(o, static_cast<std::uint16_t>(v));
    put16(o, static_cast<std::uint16_t>(v >> 16));
}
void put_fixed(std::string &o, const std::string &s, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) o.push_back(i < s.size() ? s[i] : '\0');
}
void put_mac(std::string &o, const Mac &m) { o.append(reinterpret_cast<const char *>(m.data()), 6); }

std::mutex g_rx_mutex;
std::deque<std::vector<std::uint8_t>> g_rx;
std::mutex g_tx_mutex;
int g_relay = -1;
std::atomic<bool> g_started{false};
std::atomic<std::uint64_t> g_sent{0}, g_received{0}, g_dhcp{0};
// The group the relay puts this adapter in (set_group: GT4 Online's race rooms). Only players in the
// same group see each other. A change reconnects with the new group.
std::mutex g_group_mutex;
std::string g_group;
bool g_group_set = false;
std::atomic<bool> g_regroup{false};

std::string current_group() {
    std::lock_guard lock(g_group_mutex);
    return g_group_set ? g_group : config().group;
}

// PS2_NET_TRACE=1: one line per frame (direction, ARP / IPv4 protocol, addresses and ports).
const bool g_trace = [] { const char *v = std::getenv("PS2_NET_TRACE"); return v && *v == '1'; }();

void trace_frame(const char *dir, const std::uint8_t *f, std::size_t n) {
    if (!g_trace || n < 14) return;
    const unsigned type = (f[12] << 8) | f[13];
    char what[320];
    if (type == 0x0806 && n >= 42) {
        std::snprintf(what, sizeof what, "ARP op %u %u.%u.%u.%u -> %u.%u.%u.%u", (f[20] << 8) | f[21], f[28], f[29], f[30], f[31],
                      f[38], f[39], f[40], f[41]);
    } else if (type == 0x0800 && n >= 34) {
        const std::uint8_t *ip = f + 14;
        const std::size_t ihl = (ip[0] & 15u) * 4u;
        unsigned sp = 0, dp = 0;
        if ((ip[9] == 6 || ip[9] == 17) && n >= 14 + ihl + 4) {
            sp = (ip[ihl] << 8) | ip[ihl + 1];
            dp = (ip[ihl + 2] << 8) | ip[ihl + 3];
        }
        int at = std::snprintf(what, sizeof what, "IP proto %u %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u len %u", ip[9], ip[12], ip[13], ip[14],
                               ip[15], sp, ip[16], ip[17], ip[18], ip[19], dp, (ip[2] << 8) | ip[3]);
        if (ip[9] == 17) {  // UDP payload, up to 24 bytes
            const std::size_t from = 14 + ihl + 8, len = (ip[2] << 8) | ip[3];
            const std::size_t end = std::min<std::size_t>({n, 14 + len, from + 24});
            at += std::snprintf(what + at, sizeof what - static_cast<std::size_t>(at), " |");
            for (std::size_t i = from; i < end && at < static_cast<int>(sizeof what) - 4; ++i)
                at += std::snprintf(what + at, sizeof what - static_cast<std::size_t>(at), " %02x", f[i]);
        }
    } else {
        std::snprintf(what, sizeof what, "type 0x%04X", type);
    }
    std::fprintf(stderr, "[net] %s %zu bytes, dst %02x:%02x:%02x:%02x:%02x:%02x: %s\n", dir, n, f[0], f[1], f[2], f[3], f[4], f[5], what);
}

#ifndef __EMSCRIPTEN__
int dial(const std::string &host, const std::string &port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *list = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &list) != 0) return -1;
    int s = -1;
    for (addrinfo *a = list; a; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s < 0) continue;
        // Connect with a 5 s limit: a server that is down (no RST, packets dropped) would otherwise
        // block for minutes and the backup would never be tried.
        const int flags = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, flags | O_NONBLOCK);
        int rc = connect(s, a->ai_addr, a->ai_addrlen);
        if (rc != 0 && errno == EINPROGRESS) {
            pollfd p{s, POLLOUT, 0};
            int err = 0;
            socklen_t len = sizeof err;
            rc = (poll(&p, 1, 5000) == 1 && getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) ? 0 : -1;
        }
        fcntl(s, F_SETFL, flags);
        if (rc == 0) break;
        close(s);
        s = -1;
    }
    freeaddrinfo(list);
    if (s >= 0) {
        const int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    }
    return s;
}

bool send_all(int s, const std::string &data) {
    std::size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = ::send(s, data.data() + done, data.size() - done, MSG_NOSIGNAL);
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

void run() {
    const Config &c = config();
    for (;;) {
        // Main server first, then the backups in order.
        int ctl = -1, relay = -1;
        const Server *used = nullptr;
        for (const Server &s : c.servers) {
            ctl = dial(s.host, s.ctl_port);
            relay = ctl >= 0 ? dial(s.host, s.relay_port) : -1;
            if (ctl >= 0 && relay >= 0) {
                used = &s;
                break;
            }
            std::fprintf(stderr, "[net] cannot reach the ad hoc server %s:%s%s\n", s.host.c_str(), s.ctl_port.c_str(),
                         &s != &c.servers.back() ? " - trying the backup" : ", retrying");
            if (ctl >= 0) close(ctl);
            if (relay >= 0) close(relay);
            ctl = relay = -1;
        }
        if (!used) {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }
        std::string login;
        login.push_back(static_cast<char>(kCtlLogin));
        put_mac(login, c.mac);
        put_fixed(login, c.name.substr(0, kNicknameLength - 1), kNicknameLength);
        put_fixed(login, c.product, kProductLength);
        login.push_back(static_cast<char>(kCtlConnect));
        const std::string group = current_group();
        g_regroup = false;
        put_fixed(login, group, kGroupLength);
        std::string init;
        put32(init, 0);  // datagram socket
        put_mac(init, c.mac);
        put16(init, 0);
        put16(init, kFramePort);
        init.append(10, '\0');
        bool ok = send_all(ctl, login) && send_all(relay, init);
        if (ok) {
            std::lock_guard lock(g_tx_mutex);
            g_relay = relay;
        }
        std::fprintf(stderr, "[net] %s to %s:%s as %s (%02x:%02x:%02x:%02x:%02x:%02x), group %s\n",
                     ok ? "connected" : "login failed", used->host.c_str(), used->ctl_port.c_str(), c.name.c_str(), c.mac[0],
                     c.mac[1], c.mac[2], c.mac[3], c.mac[4], c.mac[5], group.c_str());
        std::string in;
        auto last_ping = std::chrono::steady_clock::now();
        while (ok) {
            if (g_regroup.load()) break;
            pollfd fds[2] = {{ctl, POLLIN, 0}, {relay, POLLIN, 0}};
            if (poll(fds, 2, 250) < 0) break;
            char buf[16384];
            if (fds[0].revents) {
                // Join/leave notices and pings: the adapter needs none of them.
                if (recv(ctl, buf, sizeof buf, 0) <= 0) break;
            }
            if (fds[1].revents) {
                const ssize_t n = recv(relay, buf, sizeof buf, 0);
                if (n <= 0) break;
                in.append(buf, static_cast<std::size_t>(n));
                std::size_t off = 0;
                while (in.size() - off >= kPdpHeader) {
                    const auto *h = reinterpret_cast<const std::uint8_t *>(in.data() + off);
                    const std::uint32_t size = h[10] | (h[11] << 8) | (h[12] << 16) | (static_cast<std::uint32_t>(h[13]) << 24);
                    if (size > 65536) { ok = false; break; }
                    if (in.size() - off - kPdpHeader < size) break;
                    std::vector<std::uint8_t> frame(h + kPdpHeader, h + kPdpHeader + size);
                    trace_frame("in ", frame.data(), frame.size());
                    off += kPdpHeader + size;
                    ++g_received;
                    std::lock_guard lock(g_rx_mutex);
                    if (g_rx.size() < kRxQueueMax) g_rx.push_back(std::move(frame));
                }
                in.erase(0, off);
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - last_ping > std::chrono::seconds(5)) {
                last_ping = now;
                if (!send_all(ctl, std::string(1, static_cast<char>(kCtlPing)))) break;
                static std::uint64_t shown_sent = 0, shown_received = 0;
                if (g_sent != shown_sent || g_received != shown_received) {
                    shown_sent = g_sent;
                    shown_received = g_received;
                    std::fprintf(stderr, "[net] frames sent %llu, received %llu\n", static_cast<unsigned long long>(shown_sent),
                                 static_cast<unsigned long long>(shown_received));
                }
            }
        }
        {
            std::lock_guard lock(g_tx_mutex);
            g_relay = -1;
        }
        close(ctl);
        close(relay);
        if (g_regroup.load()) {
            std::lock_guard lock(g_rx_mutex);
            g_rx.clear();  // frames from the old group
            continue;
        }
        std::fprintf(stderr, "[net] connection to the ad hoc server lost (sent %llu, received %llu frames), reconnecting\n",
                     static_cast<unsigned long long>(g_sent.load()), static_cast<unsigned long long>(g_received.load()));
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}
#endif

}

bool enabled() {
#ifdef __EMSCRIPTEN__
    return false;
#else
    return config().enabled;
#endif
}

Mac mac() { return config().mac; }

void set_group(const std::string &group) {
    const std::string g = (group.empty() ? config().group : group).substr(0, kGroupLength);
    {
        std::lock_guard lock(g_group_mutex);
        if (g_group_set && g == g_group) return;
        if (!g_group_set && g == config().group) {
            g_group = g;
            g_group_set = true;
            return;
        }
        g_group = g;
        g_group_set = true;
    }
    std::fprintf(stderr, "[net] switching to group %s\n", g.c_str());
    g_regroup = true;
}

std::string group() { return current_group(); }
std::string default_group() { return config().group; }

std::uint64_t dhcp_requests() { return g_dhcp.load(); }

bool connected()
{
    std::lock_guard lock(g_tx_mutex);
    return g_relay >= 0;
}

void start() {
#ifndef __EMSCRIPTEN__
    if (!enabled() || g_started.exchange(true)) return;
    std::thread(run).detach();
#endif
}

void inject(std::vector<std::uint8_t> frame) {
    std::lock_guard lock(g_rx_mutex);
    if (g_rx.size() < kRxQueueMax) g_rx.push_back(std::move(frame));
}

void send(const std::uint8_t *frame, std::size_t size) {
#ifndef __EMSCRIPTEN__
    if (size < 14) return;
    trace_frame("out", frame, size);
    // DHCP is answered here, by the adapter itself (dhcp.hpp): the relay has no router behind it.
    if (std::vector<std::uint8_t> reply; dhcp::handle(frame, size, config().mac, reply)) {
        ++g_dhcp;
        inject(std::move(reply));
        return;
    }
    std::string out;
    Mac dest;
    std::memcpy(dest.data(), frame, 6);
    // Broadcast and multicast go to the whole group.
    if (dest[0] & 1u) dest.fill(0xFF);
    put_mac(out, dest);
    put16(out, 0);
    put16(out, kFramePort);
    put32(out, static_cast<std::uint32_t>(size));
    out.append(reinterpret_cast<const char *>(frame), size);
    std::lock_guard lock(g_tx_mutex);
    if (g_relay >= 0 && send_all(g_relay, out)) ++g_sent;
#else
    (void)frame;
    (void)size;
#endif
}

bool receive(std::vector<std::uint8_t> &frame) {
    std::lock_guard lock(g_rx_mutex);
    if (g_rx.empty()) return false;
    frame = std::move(g_rx.front());
    g_rx.pop_front();
    return true;
}

}

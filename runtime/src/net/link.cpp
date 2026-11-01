// SPDX-License-Identifier: GPL-3.0-or-later
#include "link.hpp"

#include "dhcp.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include <unistd.h>

#ifndef __EMSCRIPTEN__
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

struct Config {
    bool enabled = false;
    std::string host;
    std::string ctl_port = "27312", relay_port = "27313";
    std::string group = "GT4LAN", name, product = "SCUS97436";
    Mac mac{};
};

const Config &config() {
    static const Config c = [] {
        Config c;
        const char *server = std::getenv("PS2_NET_SERVER");
        if (!server || !*server) return c;
        c.enabled = true;
        c.host = server;
        if (const auto colon = c.host.rfind(':'); colon != std::string::npos) {
            const int port = std::atoi(c.host.c_str() + colon + 1);
            c.host.resize(colon);
            if (port > 0 && port < 65535) {
                c.ctl_port = std::to_string(port);
                c.relay_port = std::to_string(port + 1);
            }
        }
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
std::atomic<std::uint64_t> g_sent{0}, g_received{0};

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
        if (connect(s, a->ai_addr, a->ai_addrlen) == 0) break;
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
        const int ctl = dial(c.host, c.ctl_port);
        int relay = ctl >= 0 ? dial(c.host, c.relay_port) : -1;
        if (ctl < 0 || relay < 0) {
            std::fprintf(stderr, "[net] cannot reach the ad hoc server %s:%s, retrying\n", c.host.c_str(),
                         c.ctl_port.c_str());
            if (ctl >= 0) close(ctl);
            if (relay >= 0) close(relay);
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }
        std::string login;
        login.push_back(static_cast<char>(kCtlLogin));
        put_mac(login, c.mac);
        put_fixed(login, c.name.substr(0, kNicknameLength - 1), kNicknameLength);
        put_fixed(login, c.product, kProductLength);
        login.push_back(static_cast<char>(kCtlConnect));
        put_fixed(login, c.group, kGroupLength);
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
                     ok ? "connected" : "login failed", c.host.c_str(), c.ctl_port.c_str(), c.name.c_str(), c.mac[0],
                     c.mac[1], c.mac[2], c.mac[3], c.mac[4], c.mac[5], c.group.c_str());
        std::string in;
        auto last_ping = std::chrono::steady_clock::now();
        while (ok) {
            pollfd fds[2] = {{ctl, POLLIN, 0}, {relay, POLLIN, 0}};
            if (poll(fds, 2, 1000) < 0) break;
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
            }
        }
        {
            std::lock_guard lock(g_tx_mutex);
            g_relay = -1;
        }
        close(ctl);
        close(relay);
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
    // DHCP is answered here, by the adapter itself (dhcp.hpp): the relay has no router behind it.
    if (std::vector<std::uint8_t> reply; dhcp::handle(frame, size, config().mac, reply)) {
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

// SPDX-License-Identifier: GPL-3.0-or-later
// GT4 Online lap results, see gt4_results.h.
//
// Where the data comes from (retail SCUS-97328, addresses from gustavocanepa/gt4-decomp's
// config/symbol_addrs.txt, checked in PC races):
//   DynamicsConductor::ProcessControlLine(this, car, lap, total_ticks, lap_ms, ...) runs when a car
//     crosses the line and finishes a lap: a1 = car index, a2 = finished lap (1..), a3 = race time
//     in 1/3000 s, t0 = lap time in ms. (Tsukuba, car 4: a3 = 224009, t0 = 74669 = 1'14.669.)
//   ProcessCheckPoint(this, car, sector, lap0, ...) and ProcessLap0ControlLine(this, car, ...)
//     (start of the race) only feed the live status.
//   The conductor's class (vtable at this+0x10140, GCC 2.96 puts the vptr after the members) gives
//   the race mode.
//   RaceSpec: static pointer at 0x622F4C. +0x04 course label, +0x44 race label, +0xC4 entry count,
//     entries at +0xC8 (0x68 each: +0x04 = 1 for the player's car, +0x10 car id, +0x18 car label),
//     +0x350 the GT4 save name (the name typed at the first start).
#include "gt4_results.h"

#include "ps2/memory.hpp"
#include "ps2/net.hpp"
#include "ps2/system.hpp"

#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace gt4::results {

namespace {

#include "gt4_names.inc"

// ------------------------------------------------------------------ game memory

constexpr std::uint32_t kRaceSpecPtr = 0x00622F4Cu;
constexpr std::uint32_t kConductorVptr = 0x10140u;
// LAN settings object (Options > LAN Settings), a heap object at a fixed place in this build
// (same address in every PC state, boot to race): vptr 0x687CD0 at +0, console name at +4
// (64 bytes), Host at +0x98 (1 = Yes), No. of Players at +0xA0. Found by diffing a host state
// (Host Yes, 2 players) against a client and a default one.
constexpr std::uint32_t kLanSettings = 0x00A46C04u, kLanSettingsVtable = 0x00687CD0u;

std::uint32_t rd32(std::uint32_t a) {
    a &= ps2::kRamSize - 1;
    if (a > ps2::kRamSize - 4) return 0;
    return ps2::load<std::uint32_t>(ps2::g_ram + a);
}

// A NUL-terminated string of printable ASCII from EE RAM (empty if anything looks wrong).
std::string rdstr(std::uint32_t a, std::size_t max) {
    std::string s;
    a &= ps2::kRamSize - 1;
    for (std::size_t i = 0; i < max && a + i < ps2::kRamSize; ++i) {
        const unsigned char c = ps2::g_ram[a + i];
        if (c == 0) return s;
        if (c < 0x20 || c > 0x7E) return {};
        s += static_cast<char>(c);
    }
    return {};
}

const char *lookup(const NamePair *table, std::size_t n, const std::string &label) {
    for (std::size_t i = 0; i < n; ++i)
        if (label == table[i].label) return table[i].name;
    return nullptr;
}

std::string course_name(const std::string &label) {
    if (const char *n = lookup(kCourseNames, sizeof kCourseNames / sizeof *kCourseNames, label)) return n;
    return label;
}

std::string car_name(const std::string &label) {
    if (const char *n = lookup(kCarNames, sizeof kCarNames / sizeof *kCarNames, label)) return n;
    return label;
}

struct Race {
    std::string mode;          // arcade, timetrial, gtmode, lan, license, mission, other
    std::string course_label;  // tsukuba
    std::string race_label;    // sample_entry_6
    std::string player;        // GT4 save name
    int player_car = -1;       // entry index of the player's car
    std::string car_label;     // prius_g_touring_03
};

std::string mode_of(std::uint32_t conductor, const std::string &race_label) {
    switch (rd32(conductor + kConductorVptr)) {
    case 0x00679CE8u:  // DynamicsConductorSinglePlayer: Arcade single races and GT Mode events
        return race_label.rfind("sample_entry", 0) == 0 ? "arcade" : "gtmode";
    case 0x00679988u: return "arcade";     // Battle2P (split screen)
    case 0x00679B38u: return "lan";        // BattleMP
    case 0x00685890u: return "timetrial";  // FreeRun
    case 0x006856E0u: return "gtmode";     // FreePractice
    case 0x00685A40u: return "license";
    case 0x00685DA0u:                      // Mission
    case 0x00685F50u: return "mission";    // Training
    default: return "other";
    }
}

int g_force_car = -1;

// The last race setup seen (course + player's car): a LAN race fills RaceSpec on the grid and
// clears it when the race starts, so the room listing keeps the track from the grid.
std::mutex g_seen_mutex;
std::string g_seen_course, g_seen_car;

void remember(const std::string &course, const std::string &car) {
    if (course.empty()) return;
    std::lock_guard<std::mutex> lock(g_seen_mutex);
    g_seen_course = course;
    if (!car.empty()) g_seen_car = car;
}

// Steady-clock second of the last line crossing of ANY car (race running, also in LAN races where
// the player's car index isn't known).
std::atomic<long long> g_race_activity{-1000};

Race read_race(std::uint32_t conductor) {
    Race r;
    const std::uint32_t spec = rd32(kRaceSpecPtr);
    if (spec) {
        r.course_label = rdstr(spec + 0x04, 64);
        r.race_label = rdstr(spec + 0x44, 64);
        r.player = rdstr(spec + 0x350, 32);
        const std::uint32_t n = rd32(spec + 0xC4);
        for (std::uint32_t i = 0; i < n && i < 32; ++i) {
            const std::uint32_t e = spec + 0xC8 + i * 0x68;
            if (g_force_car >= 0 ? static_cast<int>(i) == g_force_car : rd32(e + 0x04) == 1) {
                r.player_car = static_cast<int>(i);
                r.car_label = rdstr(e + 0x18, 64);
                break;
            }
        }
    }
    if (r.player.empty())
        if (const char *p = std::getenv("PS2_PLAYER_NAME")) r.player = p;
    r.mode = mode_of(conductor, r.race_label);
    remember(r.course_label, r.car_label);
    return r;
}

long long now_s() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ------------------------------------------------------------------ JSON and sending

std::string json_str(const std::string &s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            o += '\\';
            o += static_cast<char>(c);
        } else if (c < 0x20) {
            char b[8];
            std::snprintf(b, sizeof b, "\\u%04x", c);
            o += b;
        } else {
            o += static_cast<char>(c);
        }
    }
    return o + "\"";
}

struct Report {
    std::string body;
    bool lap = false;  // laps are retried, statuses are not
    int tries = 0;
};

std::mutex g_mutex;
std::condition_variable g_wake;
std::deque<Report> g_queue;
std::string g_host, g_port = "27314";
std::string g_device = "Quest";
std::string g_lap_log;

// Ping to the server (gt4::results::pingMs): the last 5 TCP handshake times.
std::mutex g_ping_mutex;
std::deque<int> g_ping_samples;

// One POST; returns the HTTP status (0 = no connection or no answer).
int post(const std::string &body) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(g_host.c_str(), g_port.c_str(), &hints, &res) != 0 || !res) return 0;
    int status = 0;
    for (addrinfo *a = res; a && !status; a = a->ai_next) {
        const int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        timeval tv{5, 0};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        const auto t0 = std::chrono::steady_clock::now();
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) {
            notePing(static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()));
            const std::string req = "POST /api/report HTTP/1.1\r\nHost: " + g_host +
                                    "\r\nContent-Type: application/json\r\nContent-Length: " +
                                    std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
            if (send(fd, req.data(), req.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(req.size())) {
                char buf[64] = {};
                const ssize_t got = recv(fd, buf, sizeof buf - 1, 0);
                if (got > 12 && std::strncmp(buf, "HTTP/1.", 7) == 0) status = std::atoi(buf + 9);
            }
        }
        close(fd);
    }
    freeaddrinfo(res);
    return status;
}

void sender() {
    for (;;) {
        Report r;
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            g_wake.wait(lock, [] { return !g_queue.empty(); });
            r = std::move(g_queue.front());
            g_queue.pop_front();
        }
        const int status = post(r.body);
        if (r.lap || status != 200) std::fprintf(stderr, "[results] %s -> %d\n", r.lap ? "lap" : "status", status);
        // Laps: retry when the server wasn't reached or was busy (0, 429, 5xx), for ~10 minutes.
        // A 400 means the server rejected the lap itself: drop it.
        if (r.lap && (status == 0 || status == 429 || status >= 500) && ++r.tries < 20) {
            std::this_thread::sleep_for(std::chrono::seconds(std::min(30, 2 << std::min(r.tries, 4))));
            std::lock_guard<std::mutex> lock(g_mutex);
            g_queue.push_back(std::move(r));
        }
    }
}

void enqueue(std::string body, bool lap) {
    if (g_host.empty()) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!lap) {  // keep only the newest status
        for (auto it = g_queue.begin(); it != g_queue.end();) it = it->lap ? std::next(it) : g_queue.erase(it);
        if (g_queue.size() > 200) return;
    }
    g_queue.push_back({std::move(body), lap, 0});
    g_wake.notify_one();
}

// ------------------------------------------------------------------ race tracking

int g_log = 0;
std::uint64_t g_race_no = 0;
std::string g_race_id;
std::uint32_t g_last_lap = 0, g_last_ms = 0;
// Laps already reported this session (track|car|lap|ms). GT4's replay after a race runs the race
// again through the same conductor, so its laps arrive a second time with identical times (seen
// 2026-11-02: New York lap 1 2'02.804 reported again at the replay); two real laps never match
// to the millisecond.
std::set<std::string> g_reported;
// Last status report (steady-clock seconds; EE thread and heartbeat thread).
std::atomic<long long> g_last_status{-1000};

std::string new_race_id() {
    return g_device + "-" + std::to_string(static_cast<long long>(std::time(nullptr))) + "-" + std::to_string(++g_race_no);
}

std::mutex g_room_mutex;
Room g_room;

// In a room: room id, host flag and this player's ping (the lobby shows host + viewer ping per race;
// older servers ignore the field).
std::string room_fields() {
    std::lock_guard<std::mutex> lock(g_room_mutex);
    if (g_room.id.empty()) return {};
    std::string o = ",\"room\":" + json_str(g_room.id) + ",\"host\":" + (g_room.host ? "true" : "false");
    if (const int ms = pingMs(); ms >= 0) o += ",\"ping\":" + std::to_string(ms);
    return o;
}

std::string base_fields(const Race &r) {
    std::string o = "\"v\":1,\"device\":" + json_str(g_device) + ",\"player\":" +
                    json_str(r.player.empty() ? g_device : r.player) + ",\"mode\":" + json_str(r.mode);
    if (!r.course_label.empty()) o += ",\"track\":" + json_str(course_name(r.course_label));
    if (!r.car_label.empty()) o += ",\"car\":" + json_str(car_name(r.car_label)) + ",\"car_id\":" + json_str(r.car_label);
    return o;
}

void send_status(const Race &r, int lap, bool force) {
    const long long now = now_s();
    if (!force && now - g_last_status.load() < 5) return;
    g_last_status = now;
    std::string body = "{" + base_fields(r) + ",\"type\":\"status\",\"state\":\"race\"" + room_fields();
    if (lap > 0) body += ",\"lap\":" + std::to_string(lap);
    enqueue(body + "}", false);
}

struct LanSettings {
    bool valid = false, host = false;
    int players = 0;
    std::string console;
};

LanSettings lan_settings() {
    LanSettings s;
    if (rd32(kLanSettings) != kLanSettingsVtable) return s;
    const std::uint32_t players = rd32(kLanSettings + 0xA0), host = rd32(kLanSettings + 0x98);
    if (players < 1 || players > 6 || host > 1) return s;
    s.valid = true;
    s.host = host == 1;
    s.players = static_cast<int>(players);
    s.console = rdstr(kLanSettings + 4, 64);
    return s;
}

// While in a room: "lobby" status every 10 s unless a race status went out in that time, so the
// server keeps the room listed (it drops statuses after 30 s).
void heartbeat() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        const std::string fields = room_fields();
        if (fields.empty()) continue;
        if (now_s() - g_last_status.load() < 10) continue;
        g_last_status = now_s();
        std::string player, track, car;
        current(player, track, car);
        // A line crossing in the last 20 s = a race is running (LAN races included).
        const bool racing = now_s() - g_race_activity.load() < 20;
        std::string body = std::string("{\"v\":1,\"type\":\"status\",\"state\":") + (racing ? "\"race\"" : "\"lobby\"") +
                           ",\"device\":" + json_str(g_device) + ",\"player\":" + json_str(player.empty() ? g_device : player) +
                           fields;
        {
            std::lock_guard<std::mutex> lock(g_seen_mutex);
            if (!g_seen_course.empty()) body += ",\"track\":" + json_str(course_name(g_seen_course));
        }
        if (const LanSettings lan = lan_settings(); lan.valid && room().host)
            body += ",\"max_players\":" + std::to_string(lan.players);
        enqueue(body + "}", false);
    }
}

} // namespace

int pingMs() {
    std::lock_guard<std::mutex> lock(g_ping_mutex);
    if (g_ping_samples.empty()) return -1;
    std::vector<int> s(g_ping_samples.begin(), g_ping_samples.end());
    std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end());
    return s[s.size() / 2];
}

void notePing(int ms) {
    if (ms < 0 || ms > 9999) return;
    std::lock_guard<std::mutex> lock(g_ping_mutex);
    g_ping_samples.push_back(ms);
    while (g_ping_samples.size() > 5) g_ping_samples.pop_front();
}

namespace {

void set_room(Room room) {
    {
        std::lock_guard<std::mutex> lock(g_room_mutex);
        g_room = room;
    }
    ps2::net::set_group(room.id);
    g_last_status = -1000;  // report the change right away
    std::fprintf(stderr, "[results] room %s%s\n", room.id.empty() ? "(none)" : room.id.c_str(), room.host ? " (host)" : "");
}

void on_start(ps2::Cpu &c) {
    const Race r = read_race(c.r[4].w[0]);
    if (static_cast<int>(c.r[5].w[0]) != r.player_car) return;
    g_race_id = new_race_id();
    g_last_lap = g_last_ms = 0;
    std::fprintf(stderr, "[results] race start: %s, %s, %s, player %s (car %d)\n", r.mode.c_str(),
                 course_name(r.course_label).c_str(), car_name(r.car_label).c_str(), r.player.c_str(), r.player_car);
    send_status(r, 1, true);
}

void on_checkpoint(ps2::Cpu &c) {
    const Race r = read_race(c.r[4].w[0]);
    if (static_cast<int>(c.r[5].w[0]) != r.player_car) return;
    send_status(r, static_cast<int>(c.r[7].w[0]) + 1, false);
}

void on_lap(ps2::Cpu &c) {
    const int car = static_cast<int>(c.r[5].w[0]);
    const std::uint32_t lap = c.r[6].w[0];
    const std::uint32_t ms = c.r[8].w[0];
    const Race r = read_race(c.r[4].w[0]);
    if (car != r.player_car) return;
    if (r.mode == "license") return;  // license tests: no leaderboard times (owner 2026-11-02)
    if (lap == g_last_lap && ms == g_last_ms) return;  // a mode's override and the base both ran
    g_last_lap = lap;
    g_last_ms = ms;
    if (!g_reported.insert(r.course_label + "|" + r.car_label + "|" + std::to_string(lap) + "|" + std::to_string(ms)).second) {
        std::fprintf(stderr, "[results] lap %u (%u ms) seen before: replay, not reported\n", lap, ms);
        return;
    }
    if (g_race_id.empty()) g_race_id = new_race_id();
    std::fprintf(stderr, "[results] lap %u: %u'%02u.%03u  %s, %s, %s, %s\n", lap, ms / 60000, ms / 1000 % 60, ms % 1000,
                 r.mode.c_str(), course_name(r.course_label).c_str(), car_name(r.car_label).c_str(), r.player.c_str());
    if (!g_lap_log.empty())
        if (FILE *f = std::fopen(g_lap_log.c_str(), "a")) {
            std::fprintf(f, "%lld\t%s\t%s\t%s\t%s\tlap %u\t%u\n", static_cast<long long>(std::time(nullptr)),
                         r.player.c_str(), r.mode.c_str(), course_name(r.course_label).c_str(),
                         car_name(r.car_label).c_str(), lap, ms);
            std::fclose(f);
        }
    if (lap < 1 || lap > 999 || ms < 5000 || ms > 3600000) return;
    if (r.course_label.empty() || r.car_label.empty()) {
        std::fprintf(stderr, "[results] lap not sent: track/car unknown in this mode (%s)\n", r.mode.c_str());
        return;
    }

    enqueue("{" + base_fields(r) + ",\"type\":\"lap\",\"lap\":" + std::to_string(lap) + ",\"time_ms\":" +
                std::to_string(ms) + ",\"race\":" + json_str(g_race_id) + "}",
            true);
    send_status(r, static_cast<int>(lap) + 1, true);
}

void log_hit(const char *name, ps2::Cpu &c) {
    std::fprintf(stderr, "[gt4lap] frame %llu %s a0=%08X a1=%d a2=%d a3=%d t0=%d t1=%d t2=%d t3=%d ra=%08X\n",
                 static_cast<unsigned long long>(ps2::frame_count()), name, c.r[4].w[0], static_cast<int>(c.r[5].w[0]),
                 static_cast<int>(c.r[6].w[0]), static_cast<int>(c.r[7].w[0]), static_cast<int>(c.r[8].w[0]),
                 static_cast<int>(c.r[9].w[0]), static_cast<int>(c.r[10].w[0]), static_cast<int>(c.r[11].w[0]),
                 c.r[31].w[0]);
}

enum class Kind { Start, Checkpoint, Lap };

struct Probe {
    std::uint32_t pc;
    const char *name;
    Kind kind;
};

// Function entries. Each race mode's override is listed too: an override may not call the base.
// License tests are left out on purpose (owner 2026-11-02: no license times on the leaderboard).
constexpr Probe kProbes[] = {
    {0x00357B90u, "DynamicsConductor::ProcessLap0ControlLine", Kind::Start},
    {0x00347928u, "SinglePlayer::ProcessLap0ControlLine", Kind::Start},
    {0x003F59F8u, "FreeRun::ProcessLap0ControlLine", Kind::Start},
    {0x00357B10u, "DynamicsConductor::ProcessCheckPoint", Kind::Checkpoint},
    {0x003F5028u, "FreePractice::ProcessCheckPoint", Kind::Checkpoint},
    {0x003F5780u, "FreeRun::ProcessCheckPoint", Kind::Checkpoint},
    {0x00357B98u, "DynamicsConductor::ProcessControlLine", Kind::Lap},
    {0x00347230u, "Battle2P::ProcessControlLine", Kind::Lap},
    {0x003F5150u, "FreePractice::ProcessControlLine", Kind::Lap},
    {0x003F5B80u, "FreeRun::ProcessControlLine", Kind::Lap},
    {0x003F6C28u, "MachineTest::ProcessControlLine", Kind::Lap},
};

}

void current(std::string &player, std::string &track, std::string &car) {
    // Read from another thread while the game runs: rdstr() only accepts clean printable strings.
    player.clear(), track.clear(), car.clear();
    const std::uint32_t spec = rd32(kRaceSpecPtr);
    if (!spec) return;
    player = rdstr(spec + 0x350, 32);
    const std::string course = rdstr(spec + 0x04, 64);
    if (!course.empty()) track = course_name(course);
    std::string label;
    const std::uint32_t n = rd32(spec + 0xC4);
    for (std::uint32_t i = 0; i < n && i < 32; ++i) {
        const std::uint32_t e = spec + 0xC8 + i * 0x68;
        if (rd32(e + 0x04) == 1) {
            label = rdstr(e + 0x18, 64);
            if (!label.empty()) car = car_name(label);
            break;
        }
    }
    remember(course, label);
    if (player.empty())
        if (const char *p = std::getenv("PS2_PLAYER_NAME")) player = p;
}

Room room() {
    std::lock_guard<std::mutex> lock(g_room_mutex);
    return g_room;
}

void createRoom() {
    // R + the headset's 6-digit network id (net-id.txt, PS2_NET_NAME Quest-xxxxxx): unique per headset.
    std::string id = g_device;
    if (id.rfind("Quest-", 0) == 0) id = id.substr(6);
    std::string clean;
    for (char ch : id)
        if (std::isalnum(static_cast<unsigned char>(ch))) clean += ch;
    if (clean.size() > 6) clean = clean.substr(clean.size() - 6);
    if (clean.empty()) clean = std::to_string(std::time(nullptr) % 1000000);
    set_room({"R" + clean, true});
}

void joinRoom(const std::string &id) {
    if (id.empty()) return leaveRoom();
    set_room({id.substr(0, 8), false});
}

void leaveRoom() { set_room({}); }

// Menu screens open right now (names of mRootWindow objects, from their open/close functions).
std::mutex g_roots_mutex;
std::multiset<std::string> g_roots;
std::uint32_t g_top_root = 0;   // last TopRoot opened (title screen or GT Mode's map)

bool root_open(const char *name) { return g_roots.count(name) != 0; }

// TopRoot serves both the title screen and GT Mode's map; the title's script has initialize_optionmenu.
bool top_is_title() {
    const std::uint32_t desc = rd32(g_top_root + 0x1C);
    return rdstr(rd32(rd32(desc + 0x20) + 0x2C), 40) == "initialize_optionmenu";
}

std::string root_name(std::uint32_t root);

struct Drive {
    bool active = false;
    bool host = false;
    int players = 2;
    std::string status;
    std::deque<std::pair<std::uint16_t, int>> keys;   // (buttons, frames); 0 = pause
    std::string last_screen;
    unsigned waited = 0;      // frames since the last step without a screen change
    unsigned total = 0;       // frames since the start
    bool quit_gt = false;     // Triangle pressed on GT Mode's map: the next ConfirmDialog is "Quit GT Mode?"
};
std::mutex g_drive_mutex;
Drive g_drive;

constexpr std::uint16_t kUp = 1u << 4, kRight = 1u << 5, kDown = 1u << 6, kLeft = 1u << 7,
                        kTriangle = 1u << 12, kCross = 1u << 14;

void tap(Drive &d, std::uint16_t b) {
    d.keys.push_back({b, 6});
    d.keys.push_back({0, 30});
}

void drive_frame() {
    std::lock_guard<std::mutex> lock(g_drive_mutex);
    Drive &d = g_drive;
    if (!d.active) return;
    ++d.total;
    if (!d.keys.empty()) {
        auto &k = d.keys.front();
        ps2::set_pad_override(k.first);
        if (--k.second <= 0) d.keys.pop_front();
        return;
    }
    ps2::set_pad_override(0);
    std::string screen;
    {
        std::lock_guard<std::mutex> rl(g_roots_mutex);
        if (root_open("MatchingRoot")) screen = "lobby";
        else if (root_open("ConnectingRoot") || root_open("ConnectRoot")) screen = "connecting";
        else if (root_open("ConfirmDialog")) screen = "dialog";
        else if (root_open("GameRoot")) screen = "arcade";
        else if (root_open("HomeRoot")) screen = "home";
        else if (root_open("TopRoot")) screen = top_is_title() ? "title" : "gtmap";
        else if (root_open("MovieRoot")) screen = "movie";
    }
    if (screen == "lobby") {
        d.status = d.host ? "Lobby open - waiting for players" : "In the lobby - waiting for the host";
        std::fprintf(stderr, "[drive] lobby reached after %u frames\n", d.total);
        d.active = false;
        return;
    }
    if (d.total > 60 * 150) {
        d.status = "Could not reach the LAN lobby (open the GT4 title screen and try again)";
        std::fprintf(stderr, "[drive] gave up on screen '%s'\n", screen.c_str());
        d.active = false;
        return;
    }
    if (screen == d.last_screen && ++d.waited < 240) return;   // the last step still takes effect
    if (screen != d.last_screen) std::fprintf(stderr, "[drive] f%llu screen %s\n",
                                              static_cast<unsigned long long>(ps2::frame_count()), screen.c_str());
    d.last_screen = screen;
    d.waited = 0;
    if (screen == "connecting") { d.status = "Connecting..."; return; }
    if (screen == "movie") { d.status = "Skipping the intro..."; tap(d, 1u << 3); return; }
    if (screen == "home") { d.status = "Leaving GT Mode..."; tap(d, kTriangle); return; }
    if (screen == "gtmap") { d.status = "Leaving GT Mode..."; d.quit_gt = true; tap(d, kTriangle); return; }
    if (screen == "dialog") {
        if (d.quit_gt) tap(d, kCross);   // "Quit GT Mode?" - OK is preselected
        return;
    }
    if (screen == "title") {
        d.status = "Opening Arcade Mode...";
        for (int i = 0; i < 4; ++i) tap(d, kUp);   // Arcade Mode is the end of the list
        tap(d, kCross);
        return;
    }
    if (screen == "arcade") {
        d.status = "Opening Multi-LAN Race...";
        setLanSettings(d.host, d.players);
        for (int i = 0; i < 3; ++i) tap(d, kUp);
        for (int i = 0; i < 3; ++i) tap(d, kLeft);
        tap(d, kDown);
        tap(d, kRight);
        tap(d, kCross);
        return;
    }
    d.status = "Waiting for a menu...";
}

bool setLanSettings(bool host, int players) {
    if (rd32(kLanSettings) != kLanSettingsVtable || players < 1 || players > 6) return false;
    const std::uint32_t h = host ? 1u : 0u, n = static_cast<std::uint32_t>(players);
    std::memcpy(ps2::g_ram + kLanSettings + 0x98, &h, 4);
    std::memcpy(ps2::g_ram + kLanSettings + 0xA0, &n, 4);
    std::fprintf(stderr, "[results] LAN settings set: host %s, %d players\n", host ? "yes" : "no", players);
    return true;
}

// GT4's menu screens are mRootWindow objects (vtable 0x665418); the name sits at *(obj+0x1C)+0x2C
// ("GameRoot", "MatchingRoot", ...). Measured on PC states 2026-11-07.
std::string root_name(std::uint32_t root) {
    if (rd32(root + 4) != 0x00665418u) return {};
    return rdstr(rd32(rd32(root + 0x1C) + 0x2C), 48);
}

// PS2_GT4_ROOTLOG=1 (research): which screen each mRootWindow function runs on, logged when it changes.
struct RootProbe { std::uint32_t pc; const char *name; };
const RootProbe kRootProbes[] = {
    {0x0022FA08u, "closePage"}, {0x00235680u, "onKeyPress"}, {0x00235A20u, "onEvent"},
    {0x00233E38u, "v61"}, {0x00234790u, "v51"}, {0x00234940u, "v63"}, {0x00234A98u, "v64"},
    {0x00234BC0u, "v67"}, {0x00235270u, "v68"}, {0x00234D60u, "v89"}, {0x002353A8u, "v90"},
};

void startLobbyDrive(bool host, int players) {
    std::lock_guard<std::mutex> lock(g_drive_mutex);
    g_drive = Drive{};
    g_drive.active = true;
    g_drive.host = host;
    g_drive.players = std::clamp(players, 1, 6);
    g_drive.status = "Starting...";
    std::fprintf(stderr, "[drive] start: %s, %d players\n", host ? "host" : "join", g_drive.players);
}

void stopLobbyDrive() {
    std::lock_guard<std::mutex> lock(g_drive_mutex);
    g_drive = Drive{};
    ps2::set_pad_override(0);
}

std::string lobbyDriveStatus() {
    std::lock_guard<std::mutex> lock(g_drive_mutex);
    return g_drive.status;
}

bool lanSettings(bool &host, int &players) {
    const LanSettings s = lan_settings();
    host = s.host;
    players = s.players;
    return s.valid;
}

void install() {
    if (const char *v = std::getenv("PS2_GT4_LAPLOG")) g_log = std::atoi(v);
    if (const char *v = std::getenv("PS2_GT4_RESULTS_CAR")) g_force_car = std::atoi(v);
    if (const char *v = std::getenv("PS2_NET_NAME"); v && *v) g_device = v;
    if (const char *v = std::getenv("PS2_RESULTS_LOG")) g_lap_log = v;
    if (const char *v = std::getenv("PS2_RESULTS_SERVER"); v && *v && std::strcmp(v, "off") != 0) {
        g_host = v;
        if (const auto colon = g_host.rfind(':'); colon != std::string::npos) {
            g_port = g_host.substr(colon + 1);
            g_host.resize(colon);
        }
        std::thread(sender).detach();
        std::thread(heartbeat).detach();
        // Tests (and a fixed room per config.txt): PS2_ROOM=create opens this headset's race,
        // PS2_ROOM=<id> joins one.
        if (const char *room = std::getenv("PS2_ROOM"); room && *room) {
            if (std::strcmp(room, "create") == 0) createRoom();
            else joinRoom(room);
        }
        std::fprintf(stderr, "[results] reporting laps to %s:%s as %s\n", g_host.c_str(), g_port.c_str(), g_device.c_str());
    }
    // Test: LAN Settings written as soon as the object exists (then checked once a second, so a
    // value the game reloads from the save is overwritten again and shows in the log).
    if (const char *h = std::getenv("PS2_LAN_HOST"); h && *h) {
        const bool host = std::atoi(h) != 0;
        const int players = std::getenv("PS2_LAN_PLAYERS") ? std::atoi(std::getenv("PS2_LAN_PLAYERS")) : 2;
        auto previous = ps2::g_options.on_vblank;
        ps2::g_options.on_vblank = [previous, host, players] {
            if (previous) previous();
            static unsigned n = 0;
            if (++n % 60) return;
            bool h0 = false;
            int p0 = 0;
            if (lanSettings(h0, p0) && (h0 != host || p0 != players))
            {
                std::fprintf(stderr, "[results] LAN settings were host %d, %d players (frame %llu)\n", h0 ? 1 : 0, p0,
                             static_cast<unsigned long long>(ps2::frame_count()));
                setLanSettings(host, players);
            }
        };
    }
    // Open menu screens (mRootWindow open/close = vtable slots 67/68), for the lobby drive.
    ps2::set_probe_hook(0x00234BC0u, [](ps2::Cpu &c) {
        const std::uint32_t obj = c.r[4].w[0];
        const std::string n = root_name(obj);
        if (n.empty()) return;
        std::lock_guard<std::mutex> rl(g_roots_mutex);
        g_roots.insert(n);
        if (n == "TopRoot") g_top_root = obj;
    });
    ps2::set_probe_hook(0x00235270u, [](ps2::Cpu &c) {
        const std::string n = root_name(c.r[4].w[0]);
        std::lock_guard<std::mutex> rl(g_roots_mutex);
        if (auto it = g_roots.find(n); it != g_roots.end()) g_roots.erase(it);
    });
    {
        auto previous = ps2::g_options.on_vblank;
        ps2::g_options.on_vblank = [previous] {
            if (previous) previous();
            drive_frame();
        };
    }
    if (const char *a = std::getenv("PS2_LAN_AUTO"); a && *a)
        startLobbyDrive(std::strcmp(a, "join") != 0, std::getenv("PS2_LAN_PLAYERS") ? std::atoi(std::getenv("PS2_LAN_PLAYERS")) : 2);
    if (std::getenv("PS2_GT4_ROOTLOG")) {
        for (const RootProbe &rp : kRootProbes) {
            ps2::set_probe_hook(rp.pc, [&rp](ps2::Cpu &c) {
                static std::uint32_t last[32] = {};
                static unsigned long long count[32] = {};
                const std::size_t i = static_cast<std::size_t>(&rp - kRootProbes);
                const std::uint32_t obj = rp.pc == 0x0022FA08u ? c.r[5].w[0] : c.r[4].w[0];
                ++count[i];
                if (obj != last[i]) {
                    last[i] = obj;
                    std::fprintf(stderr, "[root] f%llu %s %08X %s (calls %llu)\n",
                                 static_cast<unsigned long long>(ps2::frame_count()), rp.name, obj,
                                 root_name(obj).c_str(), count[i]);
                }
            });
        }
    }
    for (const Probe &p : kProbes) {
        ps2::set_probe_hook(p.pc, [&p](ps2::Cpu &c) {
            if (g_log) log_hit(p.name, c);
            g_race_activity = now_s();
            switch (p.kind) {
            case Kind::Start: on_start(c); break;
            case Kind::Checkpoint: on_checkpoint(c); break;
            case Kind::Lap: on_lap(c); break;
            }
        });
    }
}

}

// SPDX-License-Identifier: GPL-3.0-or-later
// GT4 Online's VR online menu, see online_panel.h.
#include "online_panel.h"

#include "gt4_json.hpp"
#include "gt4_results.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../third_party/stb/stb_truetype.h"
#pragma GCC diagnostic pop

#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace gt4::panel {

namespace {

// ------------------------------------------------------------------ HTTP

std::string g_host = "217.154.211.69", g_port = "27314";

// GET path from gt4-results; returns the body, or nothing when the server can't be reached.
bool http_get(const std::string &path, std::string &body) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(g_host.c_str(), g_port.c_str(), &hints, &res) != 0 || !res) return false;
    std::string reply;
    bool ok = false;
    for (addrinfo *a = res; a && !ok; a = a->ai_next) {
        const int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        timeval tv{5, 0};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) {
            const std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + g_host + "\r\nConnection: close\r\n\r\n";
            if (send(fd, req.data(), req.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(req.size())) {
                char buf[16384];
                for (ssize_t n; (n = recv(fd, buf, sizeof buf, 0)) > 0 && reply.size() < (4u << 20);)
                    reply.append(buf, static_cast<std::size_t>(n));
                ok = reply.compare(0, 12, "HTTP/1.1 200") == 0 || reply.compare(0, 12, "HTTP/1.0 200") == 0;
            }
        }
        close(fd);
    }
    freeaddrinfo(res);
    const std::size_t head = reply.find("\r\n\r\n");
    if (!ok || head == std::string::npos) return false;
    body = reply.substr(head + 4);
    return true;
}

bool get_json(const std::string &path, json::Value &out) {
    std::string body;
    if (!http_get(path, body)) return false;
    json::Value v;
    if (!json::Reader(body.data(), body.data() + body.size()).parse(v)) return false;
    out = std::move(v);
    return true;
}

std::string url_encode(const std::string &s) {
    static const char *hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')
            o += static_cast<char>(c);
        else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 15];
        }
    }
    return o;
}

// ------------------------------------------------------------------ data

std::string str(const json::Value *v) { return v && v->type == json::Value::String ? v->str : std::string(); }
long long num(const json::Value *v, long long fallback = -1) {
    return v && v->type == json::Value::Number ? static_cast<long long>(v->num) : fallback;
}

struct Data {
    bool reached = false;  // last fetch got through
    std::time_t fetched = 0;
    json::Value live, leaderboard, recent, tracks, track, rooms;
    std::string track_name;  // the track `track` belongs to
};

std::mutex g_mutex;
std::condition_variable g_wake;
Data g_data;
std::atomic<bool> g_open{false}, g_available{false}, g_dirty{true}, g_refresh{false};
ContextFn g_context = nullptr;
int g_tab = 0;
int g_scroll[8] = {};
int g_sel = 0;  // selected row on the Races tab

enum Tab { kRaces, kLive, kLeaderboard, kTrack, kRecent, kTabCount };
const char *const kTabNames[kTabCount] = {"Races", "Live", "Leaderboard", "Track", "Recent"};

void fetch_loop() {
    std::chrono::steady_clock::time_point last_all{};
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            g_wake.wait_for(lock, std::chrono::seconds(5), [] { return g_open.load(); });
        }
        if (!g_open.load()) continue;
        const Context ctx = g_context ? g_context() : Context{};
        Data d;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            d = g_data;
        }
        const auto now = std::chrono::steady_clock::now();
        bool reached = get_json("/api/live", d.live);
        if (reached) get_json("/api/rooms", d.rooms);
        if (reached && (now - last_all > std::chrono::seconds(15) || d.leaderboard.type == json::Value::Null)) {
            reached = get_json("/api/leaderboard", d.leaderboard) && get_json("/api/recent", d.recent) &&
                      get_json("/api/tracks", d.tracks);
            if (reached) last_all = now;
        }
        // Track tab: the track being driven, else the latest lap's track.
        std::string track = ctx.track;
        if (track.empty() && d.recent.type == json::Value::Array && !d.recent.arr.empty())
            track = str(d.recent.arr[0].get("track"));
        if (reached && !track.empty()) {
            if (get_json("/api/track?name=" + url_encode(track), d.track)) d.track_name = track;
        }
        d.reached = reached;
        d.fetched = std::time(nullptr);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_data = std::move(d);
        }
        g_dirty = true;
        // While open: Live every 5 s (the wait above), the rest every 15 s.
        std::unique_lock<std::mutex> lock(g_mutex);
        g_wake.wait_for(lock, std::chrono::seconds(5), [] { return !g_open.load() || g_refresh.load(); });
        if (g_refresh.exchange(false)) {
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(2500));  // the room's first heartbeat
        }
    }
}

// ------------------------------------------------------------------ drawing

stbtt_fontinfo g_font;
std::vector<unsigned char> g_font_data;

struct Glyph {
    int w = 0, h = 0, xoff = 0, yoff = 0;
    float advance = 0;
    std::vector<unsigned char> alpha;
};
std::unordered_map<std::uint64_t, Glyph> g_glyphs;

const Glyph &glyph(int px, std::uint32_t cp) {
    const std::uint64_t key = (static_cast<std::uint64_t>(px) << 32) | cp;
    auto it = g_glyphs.find(key);
    if (it != g_glyphs.end()) return it->second;
    Glyph g;
    const float scale = stbtt_ScaleForPixelHeight(&g_font, static_cast<float>(px));
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&g_font, static_cast<int>(cp), &adv, &lsb);
    g.advance = adv * scale;
    unsigned char *bm = stbtt_GetCodepointBitmap(&g_font, 0, scale, static_cast<int>(cp), &g.w, &g.h, &g.xoff, &g.yoff);
    if (bm) {
        g.alpha.assign(bm, bm + g.w * g.h);
        stbtt_FreeBitmap(bm, nullptr);
    }
    return g_glyphs.emplace(key, std::move(g)).first->second;
}

std::vector<std::uint32_t> utf8(const std::string &s) {
    std::vector<std::uint32_t> out;
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        int n = c < 0x80 ? 0 : c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
        std::uint32_t cp = n == 0 ? c : n == 1 ? (c & 0x1F) : n == 2 ? (c & 0x0F) : (c & 0x07);
        if (i + n >= s.size() + (n ? 0 : 1)) n = 0;
        for (int k = 1; k <= n && i + k < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        out.push_back(cp);
        i += static_cast<std::size_t>(n) + 1;
    }
    return out;
}

using Color = std::uint32_t;  // 0xRRGGBB
constexpr Color kBg = 0x12151C, kHeader = 0x1B2030, kRowAlt = 0x181C26, kLine = 0x2A3042;
constexpr Color kText = 0xE8ECF4, kDim = 0x8A93A8, kAccent = 0xE63946, kGold = 0xFFC857, kGreen = 0x5BD38A;
constexpr Color kSelected = 0x2B3550;

struct Canvas {
    std::vector<std::uint8_t> &px;
    void rect(int x, int y, int w, int h, Color c) {
        const int x0 = std::max(0, x), y0 = std::max(0, y), x1 = std::min(kWidth, x + w), y1 = std::min(kHeight, y + h);
        for (int yy = y0; yy < y1; ++yy)
            for (int xx = x0; xx < x1; ++xx) {
                std::uint8_t *p = &px[(static_cast<std::size_t>(yy) * kWidth + xx) * 4];
                p[0] = c >> 16, p[1] = (c >> 8) & 0xFF, p[2] = c & 0xFF, p[3] = 255;
            }
    }
    float width(const std::string &s, int size) {
        float w = 0;
        for (std::uint32_t cp : utf8(s)) w += glyph(size, cp).advance;
        return w;
    }
    // Text with its baseline at y; clipped to max_w with an ellipsis. align: 0 left, 1 right, 2 centre.
    void text(const std::string &s, int x, int y, int size, Color c, int max_w = 100000, int align = 0) {
        std::vector<std::uint32_t> cps = utf8(s);
        float w = 0;
        for (std::uint32_t cp : cps) w += glyph(size, cp).advance;
        if (w > max_w) {
            const float dots = glyph(size, 0x2026).advance;
            while (!cps.empty() && w + dots > max_w) {
                w -= glyph(size, cps.back()).advance;
                cps.pop_back();
            }
            cps.push_back(0x2026);
            w += dots;
        }
        float pen = static_cast<float>(x);
        if (align == 1) pen = x + max_w - w;
        if (align == 2) pen = x + (max_w - w) * 0.5f;
        for (std::uint32_t cp : cps) {
            const Glyph &g = glyph(size, cp);
            const int gx = static_cast<int>(pen + 0.5f) + g.xoff, gy = y + g.yoff;
            for (int yy = 0; yy < g.h; ++yy) {
                const int py = gy + yy;
                if (py < 0 || py >= kHeight) continue;
                for (int xx = 0; xx < g.w; ++xx) {
                    const int pxx = gx + xx;
                    if (pxx < 0 || pxx >= kWidth) continue;
                    const unsigned a = g.alpha[static_cast<std::size_t>(yy) * g.w + xx];
                    if (!a) continue;
                    std::uint8_t *p = &px[(static_cast<std::size_t>(py) * kWidth + pxx) * 4];
                    const unsigned cr = c >> 16, cg = (c >> 8) & 0xFF, cb = c & 0xFF;
                    p[0] = static_cast<std::uint8_t>((cr * a + p[0] * (255 - a)) / 255);
                    p[1] = static_cast<std::uint8_t>((cg * a + p[1] * (255 - a)) / 255);
                    p[2] = static_cast<std::uint8_t>((cb * a + p[2] * (255 - a)) / 255);
                }
            }
            pen += g.advance;
        }
    }
};

std::string lap_time(long long ms) {
    if (ms < 0) return "-";
    char b[32];
    std::snprintf(b, sizeof b, "%lld'%02lld.%03lld", ms / 60000, ms / 1000 % 60, ms % 1000);
    return b;
}

std::string mode_name(const std::string &m) {
    if (m == "arcade") return "Arcade";
    if (m == "timetrial") return "Time Trial";
    if (m == "gtmode") return "GT Mode";
    if (m == "lan") return "LAN";
    if (m == "license") return "License";
    if (m == "mission") return "Mission";
    return m.empty() ? "" : "Other";
}

// "2026-11-02T10:47:46Z" -> local "02.11. 11:47"
std::string short_date(const std::string &iso) {
    std::tm t{};
    if (std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec) != 6)
        return iso;
    t.tm_year -= 1900;
    t.tm_mon -= 1;
    const std::time_t utc = timegm(&t);
    std::tm local{};
    localtime_r(&utc, &local);
    char b[32];
    std::strftime(b, sizeof b, "%d.%m. %H:%M", &local);
    return b;
}

struct Column {
    const char *title;
    int x, w, align;
};

constexpr int kRowH = 46, kTableTop = 244, kRows = 10, kBody = 28;

// A table: header row, then the rows (vector of cell strings), the player's rows in gold.
// A row whose cells after the first are all empty is one wide cell (the Races tab's action row).
void table(Canvas &cv, const std::vector<Column> &cols, const std::vector<std::vector<std::string>> &rows,
           const std::vector<bool> &own, int &scroll, const char *empty, int selected = -1, int top = kTableTop) {
    const int fit = std::min(kRows, (kHeight - 56 - 50 - top) / kRowH);
    for (const Column &c : cols) cv.text(c.title, c.x, top - 14, 22, kDim, c.w, c.align);
    cv.rect(40, top - 4, kWidth - 80, 2, kLine);
    if (rows.empty()) {
        cv.text(empty, 40, top + 90, 32, kDim, kWidth - 80, 2);
        return;
    }
    if (selected >= 0) scroll = std::clamp(scroll, selected - fit + 1, selected);
    scroll = std::clamp(scroll, 0, std::max(0, static_cast<int>(rows.size()) - fit));
    for (int i = 0; i < fit && scroll + i < static_cast<int>(rows.size()); ++i) {
        const int r = scroll + i, y = top + i * kRowH;
        bool wide = rows[r].size() > 1;
        for (std::size_t k = 1; k < rows[r].size(); ++k) wide = wide && rows[r][k].empty();
        if (wide) {
            if (r == selected) {
                cv.rect(40, y, kWidth - 80, kRowH, kSelected);
                cv.rect(40, y, 6, kRowH, kAccent);
            }
            cv.text(rows[r][0], cols[0].x, y + 32, kBody, own[r] ? kGold : kText, kWidth - 100);
            continue;
        }
        if (r == selected) {
            cv.rect(40, y, kWidth - 80, kRowH, kSelected);
            cv.rect(40, y, 6, kRowH, kAccent);
        } else if (i % 2) {
            cv.rect(40, y, kWidth - 80, kRowH, kRowAlt);
        }
        const Color c = own[r] ? kGold : kText;
        for (std::size_t k = 0; k < cols.size() && k < rows[r].size(); ++k)
            cv.text(rows[r][k], cols[k].x, y + 32, kBody, k == 0 && !own[r] ? kDim : c, cols[k].w, cols[k].align);
    }
    if (static_cast<int>(rows.size()) > fit) {
        char b[48];
        std::snprintf(b, sizeof b, "%d-%d of %zu", scroll + 1, std::min(scroll + fit, static_cast<int>(rows.size())), rows.size());
        cv.text(b, kWidth - 340, top + fit * kRowH + 30, 22, kDim, 300, 1);
    }
}

// The Races tab: row 0 creates (or leaves) a race, the others are the open races to join.
struct RaceRow {
    std::vector<std::string> cells;
    std::string join;  // room id to join; "" for row 0
    bool own = false;
};

std::vector<RaceRow> race_rows(const Data &d, const gt4::results::Room &cur) {
    std::vector<RaceRow> rows;
    if (cur.id.empty())
        rows.push_back({{"+  Create a race", "", "", "", "", ""}, "", false});
    else
        rows.push_back({{std::string("x  Leave race ") + cur.id, "", "", "", "", ""}, "", true});
    if (d.rooms.type == json::Value::Array)
        for (const json::Value &v : d.rooms.arr) {
            const std::string id = str(v.get("room"));
            if (id.empty()) continue;
            const long long n = num(v.get("count"), 1);
            std::string players;
            if (const json::Value *p = v.get("players"); p && p->type == json::Value::Array)
                for (const json::Value &name : p->arr) players += (players.empty() ? "" : ", ") + str(&name);
            const long long max = num(v.get("max_players"));
            std::string state = str(v.get("state")) == "racing" ? "racing" : "waiting";
            if (state == "waiting" && max > 0 && n >= max) state = "full";
            std::string track = str(v.get("track"));
            if (track.empty()) track = "-";
            const std::string count = max > 0 ? std::to_string(n) + " / " + std::to_string(max) : std::to_string(n);
            rows.push_back({{str(v.get("host")), id, track, count, players, state}, id, id == cur.id});
        }
    return rows;
}

void draw(std::vector<std::uint8_t> &px, const Data &d, const Context &ctx) {
    Canvas cv{px};
    cv.rect(0, 0, kWidth, kHeight, kBg);
    // Header
    cv.rect(0, 0, kWidth, 96, kHeader);
    cv.rect(0, 96, kWidth, 4, kAccent);
    cv.text("GT4 ONLINE", 40, 64, 44, kText);
    cv.text("Leaderboard  ·  gt4.portroyale.online", 330, 62, 24, kDim);
    const std::string who = ctx.player.empty() ? "not signed in (start a race)" : ctx.player;
    cv.text(who, kWidth - 440, 46, 28, ctx.player.empty() ? kDim : kGold, 400, 1);
    char status[96];
    if (!d.fetched)
        std::snprintf(status, sizeof status, "connecting...");
    else if (!d.reached)
        std::snprintf(status, sizeof status, "server not reachable");
    else {
        std::tm t{};
        localtime_r(&d.fetched, &t);
        std::strftime(status, sizeof status, "online  ·  updated %H:%M:%S", &t);
    }
    cv.text(status, kWidth - 440, 80, 20, d.fetched && !d.reached ? kAccent : kGreen, 400, 1);
    // Tabs
    const int tab_w = (kWidth - 80) / kTabCount;
    for (int i = 0; i < kTabCount; ++i) {
        const int x = 40 + i * tab_w;
        cv.text(kTabNames[i], x, 150, 30, i == g_tab ? kText : kDim, tab_w, 2);
        if (i == g_tab) cv.rect(x + tab_w / 4, 162, tab_w / 2, 4, kAccent);
    }

    const auto own = [&ctx](const std::string &player) { return !ctx.player.empty() && player == ctx.player; };
    std::vector<std::vector<std::string>> rows;
    std::vector<bool> mine;
    switch (g_tab) {
    case kRaces: {
        const gt4::results::Room cur = gt4::results::room();
        const std::vector<RaceRow> rr = race_rows(d, cur);
        g_sel = std::clamp(g_sel, 0, static_cast<int>(rr.size()) - 1);
        for (const RaceRow &row : rr) {
            rows.push_back(row.cells);
            mine.push_back(row.own);
        }
        if (cur.id.empty()) {
            cv.text("You are in the open lobby: Multi-LAN Race finds everyone who is not in a race.", 40, 192, 22, kDim, kWidth - 80);
            cv.text("Create a race, or pick one below and press A to join it.", 40, 218, 22, kDim, kWidth - 80);
        } else if (cur.host) {
            cv.text("Your race " + cur.id + " is open. In GT4: Options > LAN Settings > Host: Yes, No. of Players = seats,", 40,
                    192, 22, kGold, kWidth - 80);
            cv.text("then Arcade > Multi-LAN Race > Connect. The race starts when all seats are taken.", 40, 218, 22, kGold,
                    kWidth - 80);
        } else {
            cv.text("You joined race " + cur.id + ". In GT4: Options > LAN Settings > Host: No,", 40, 192, 22, kGold, kWidth - 80);
            cv.text("then Arcade > Multi-LAN Race > Connect and wait for the host to start.", 40, 218, 22, kGold, kWidth - 80);
        }
        constexpr int kRacesTop = kTableTop + 34;
        table(cv, {{"Host", 50, 180, 0}, {"Race", 240, 130, 0}, {"Track", 380, 330, 0}, {"Players", 720, 110, 0},
                   {"Who", 840, 260, 0}, {"Status", 1110, 130, 1}},
              rows, mine, g_scroll[kRaces], "", g_sel, kRacesTop);
        if (rr.size() == 1)
            cv.text("No open races right now", 40, kRacesTop + kRowH + 60, 28, kDim, kWidth - 80, 2);
        break;
    }
    case kLive: {
        const json::Value *devs = d.live.get("devices");
        if (devs && devs->type == json::Value::Array)
            for (const json::Value &v : devs->arr) {
                const std::string state = str(v.get("state"));
                std::string where = state == "race" ? "racing" : state == "lobby" ? "in the LAN lobby" : "in the menus";
                const long long lap = num(v.get("lap")), laps = num(v.get("laps"));
                std::string lapstr = lap > 0 ? "lap " + std::to_string(lap) + (laps > 0 ? "/" + std::to_string(laps) : "") : "";
                rows.push_back({str(v.get("player")), where, mode_name(str(v.get("mode"))), str(v.get("track")),
                                str(v.get("car")), lapstr});
                mine.push_back(own(str(v.get("player"))));
            }
        table(cv, {{"Player", 50, 190, 0}, {"Status", 250, 190, 0}, {"Mode", 450, 130, 0}, {"Track", 590, 250, 0},
                   {"Car", 850, 300, 0}, {"Lap", 1160, 80, 1}},
              rows, mine, g_scroll[kLive], "No one is racing right now");
        break;
    }
    case kLeaderboard: {
        const json::Value *players = d.leaderboard.get("players");
        if (players && players->type == json::Value::Array)
            for (const json::Value &v : players->arr) {
                rows.push_back({std::to_string(num(v.get("rank"))), str(v.get("player")),
                                std::to_string(num(v.get("track_records"), 0)), std::to_string(num(v.get("car_records"), 0)),
                                std::to_string(num(v.get("laps"), 0))});
                mine.push_back(own(str(v.get("player"))));
            }
        table(cv, {{"#", 50, 60, 0}, {"Player", 120, 420, 0}, {"Track records", 560, 220, 1}, {"Car records", 800, 200, 1},
                   {"Laps", 1020, 200, 1}},
              rows, mine, g_scroll[kLeaderboard], "No laps yet - drive one!");
        break;
    }
    case kTrack: {
        const json::Value *entries = d.track.get("entries");
        if (!d.track_name.empty() && entries && entries->type == json::Value::Array) {
            for (const json::Value &v : entries->arr) {
                rows.push_back({std::to_string(num(v.get("rank"))), lap_time(num(v.get("time_ms"))), str(v.get("player")),
                                str(v.get("car")), mode_name(str(v.get("mode"))), short_date(str(v.get("date")))});
                mine.push_back(own(str(v.get("player"))));
            }
        }
        table(cv, {{"#", 50, 50, 0}, {"Time", 110, 150, 0}, {"Player", 280, 200, 0}, {"Car", 490, 420, 0},
                   {"Mode", 920, 130, 0}, {"Date", 1060, 180, 1}},
              rows, mine, g_scroll[kTrack], "No laps on this track yet");
        break;
    }
    case kRecent: {
        if (d.recent.type == json::Value::Array)
            for (const json::Value &v : d.recent.arr) {
                rows.push_back({short_date(str(v.get("date"))), lap_time(num(v.get("time_ms"))), str(v.get("player")),
                                str(v.get("track")), str(v.get("car")), mode_name(str(v.get("mode")))});
                mine.push_back(own(str(v.get("player"))));
            }
        table(cv, {{"Date", 50, 150, 0}, {"Time", 210, 140, 0}, {"Player", 360, 170, 0}, {"Track", 540, 250, 0},
                   {"Car", 800, 300, 0}, {"Mode", 1110, 130, 1}},
              rows, mine, g_scroll[kRecent], "No laps yet");
        break;
    }
    }
    // Track tab title above the table
    if (g_tab == kTrack && !d.track_name.empty())
        cv.text(d.track_name + (ctx.track == d.track_name ? "  ·  the track you are on" : "  ·  latest lap's track"), 40,
                kTableTop - 40, 24, kGold, kWidth - 80);
    // Footer
    cv.rect(0, kHeight - 56, kWidth, 56, kHeader);
    cv.text(g_tab == kRaces ? "Right stick  left/right: tab   up/down: choose     A: select     B or both stick clicks: close"
                            : "Right stick  left/right: tab   up/down: scroll        B or both stick clicks: close",
            40, kHeight - 20, 22, kDim, kWidth - 80, 2);
}

}

bool start(const std::string &server, const std::string &font, ContextFn context) {
    if (!server.empty()) {
        g_host = server;
        g_port = "27314";
        if (const auto colon = g_host.rfind(':'); colon != std::string::npos) {
            g_port = g_host.substr(colon + 1);
            g_host.resize(colon);
        }
    }
    std::ifstream in(font, std::ios::binary);
    g_font_data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (g_font_data.empty() || !stbtt_InitFont(&g_font, g_font_data.data(), stbtt_GetFontOffsetForIndex(g_font_data.data(), 0))) {
        std::fprintf(stderr, "[panel] font %s not loaded: online menu off\n", font.c_str());
        return false;
    }
    g_context = context;
    g_available = true;
    std::thread(fetch_loop).detach();
    std::fprintf(stderr, "[panel] online menu ready (%s:%s)\n", g_host.c_str(), g_port.c_str());
    return true;
}

bool available() { return g_available.load(); }

void setOpen(bool open) {
    if (!g_available.load() || open == g_open.load()) return;
    g_open = open;
    g_dirty = true;
    g_wake.notify_all();
    std::fprintf(stderr, "[panel] %s\n", open ? "open" : "closed");
}

bool isOpen() { return g_open.load(); }
void toggle() { setOpen(!g_open.load()); }

void input(int dx, int dy) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (dx) {
        g_tab = (g_tab + dx + kTabCount) % kTabCount;
        g_dirty = true;
    }
    if (dy) {
        if (g_tab == kRaces) g_sel = std::max(0, g_sel + dy);
        else g_scroll[g_tab] = std::max(0, g_scroll[g_tab] + dy);
        g_dirty = true;
    }
}

void select() {
    std::string join;
    bool row0 = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_tab != kRaces) return;
        const std::vector<RaceRow> rr = race_rows(g_data, gt4::results::room());
        if (g_sel <= 0 || g_sel >= static_cast<int>(rr.size())) row0 = true;
        else join = rr[g_sel].join;
        g_sel = 0;
    }
    const gt4::results::Room cur = gt4::results::room();
    if (row0) {
        if (cur.id.empty()) gt4::results::createRoom();
        else gt4::results::leaveRoom();
    } else if (join != cur.id) {
        gt4::results::joinRoom(join);
    }
    g_dirty = true;
    g_refresh = true;
    g_wake.notify_all();
}

void setTab(int tab) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_tab = std::clamp(tab, 0, kTabCount - 1);
    g_dirty = true;
}

bool renderBadge(std::vector<std::uint8_t> &rgba) {
    if (!g_available.load()) return false;
    constexpr int W = kBadgeWidth, H = kBadgeHeight;
    rgba.assign(static_cast<std::size_t>(W) * H * 4, 0);
    const auto put = [&](int x, int y, Color c, unsigned a) {
        if (x < 0 || y < 0 || x >= W || y >= H || !a) return;
        std::uint8_t *p = &rgba[(static_cast<std::size_t>(y) * W + x) * 4];
        const unsigned cr = c >> 16, cg = (c >> 8) & 0xFF, cb = c & 0xFF;
        // "over" onto what is there (premultiplied-free: colour blended by a, alpha accumulated)
        p[0] = static_cast<std::uint8_t>((cr * a + p[0] * (255 - a)) / 255);
        p[1] = static_cast<std::uint8_t>((cg * a + p[1] * (255 - a)) / 255);
        p[2] = static_cast<std::uint8_t>((cb * a + p[2] * (255 - a)) / 255);
        p[3] = static_cast<std::uint8_t>(a + p[3] * (255 - a) / 255);
    };
    // Pill: a rectangle with round ends (radius H/2), anti-aliased edge.
    const float r = H * 0.5f;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float cx = std::clamp(x + 0.5f, r, W - r), dx = x + 0.5f - cx, dy = y + 0.5f - r;
            const float edge = r - std::sqrt(dx * dx + dy * dy);
            const float cov = std::clamp(edge, 0.0f, 1.0f);
            put(x, y, kBg, static_cast<unsigned>(200 * cov));
        }
    // Green dot.
    const float dot_r = H * 0.17f, dot_x = r, dot_y = r;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float dx = x + 0.5f - dot_x, dy = y + 0.5f - dot_y;
            const float cov = std::clamp(dot_r - std::sqrt(dx * dx + dy * dy) + 0.5f, 0.0f, 1.0f);
            put(x, y, kGreen, static_cast<unsigned>(255 * cov));
        }
    // The word.
    std::lock_guard<std::mutex> lock(g_mutex);
    const int size = static_cast<int>(H * 0.56f);
    float pen = r + dot_r + H * 0.22f;
    const int base = static_cast<int>(H * 0.5f + size * 0.36f);
    for (std::uint32_t cp : utf8("Online")) {
        const Glyph &g = glyph(size, cp);
        const int gx = static_cast<int>(pen + 0.5f) + g.xoff, gy = base + g.yoff;
        for (int yy = 0; yy < g.h; ++yy)
            for (int xx = 0; xx < g.w; ++xx)
                put(gx + xx, gy + yy, kText, g.alpha[static_cast<std::size_t>(yy) * g.w + xx]);
        pen += g.advance;
    }
    return true;
}

bool render(std::vector<std::uint8_t> &rgba) {
    if (!g_available.load() || !g_dirty.exchange(false)) return false;
    const Context ctx = g_context ? g_context() : Context{};
    rgba.assign(static_cast<std::size_t>(kWidth) * kHeight * 4, 0);
    std::lock_guard<std::mutex> lock(g_mutex);
    draw(rgba, g_data, ctx);
    return true;
}

}

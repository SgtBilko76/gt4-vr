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
        const auto t0 = std::chrono::steady_clock::now();
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) {
            // TCP handshake = one round trip to the server (= the relay's host): the lobby's ping.
            gt4::results::notePing(static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()));
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

// ------------------------------------------------------------------ data

std::string str(const json::Value *v) { return v && v->type == json::Value::String ? v->str : std::string(); }
long long num(const json::Value *v, long long fallback = -1) {
    return v && v->type == json::Value::Number ? static_cast<long long>(v->num) : fallback;
}

struct Data {
    bool reached = false;  // last fetch got through
    std::time_t fetched = 0;
    json::Value rooms;
};

std::mutex g_mutex;
std::condition_variable g_wake;
Data g_data;
std::atomic<bool> g_open{false}, g_available{false}, g_dirty{true}, g_refresh{false};
ContextFn g_context = nullptr;
// One page: the lobby browser (owner 2026-11-10: no leaderboard / live list / track records in the
// game any more, no website either - only the open races and how to connect).
int g_scroll = 0;
int g_sel = 0;  // selected row

void fetch_loop() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            g_wake.wait_for(lock, std::chrono::seconds(5), [] { return g_open.load(); });
        }
        if (!g_open.load()) continue;
        Data d;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            d = g_data;
        }
        d.reached = get_json("/api/rooms", d.rooms);
        d.fetched = std::time(nullptr);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_data = std::move(d);
        }
        g_dirty = true;
        // While open: the race list every 5 s, at once (after the room's first heartbeat) after A.
        std::unique_lock<std::mutex> lock(g_mutex);
        g_wake.wait_for(lock, std::chrono::seconds(5), [] { return !g_open.load() || g_refresh.load(); });
        if (g_refresh.exchange(false)) {
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(2500));
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

struct Column {
    const char *title;
    int x, w, align;
};

constexpr int kRowH = 46, kTableTop = 244, kRows = 10, kBody = 28;

// A table: header row, then the rows (vector of cell strings), the player's rows in gold.
// A row whose cells after the first are all empty is one wide cell (the Races tab's action row).
void table(Canvas &cv, const std::vector<Column> &cols, const std::vector<std::vector<std::string>> &rows,
           const std::vector<bool> &own, int &scroll, const char *empty, int selected = -1, int top = kTableTop,
           Color first = kDim) {
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
            cv.text(rows[r][k], cols[k].x, y + 32, kBody, k == 0 && !own[r] ? first : c, cols[k].w, cols[k].align);
    }
    if (static_cast<int>(rows.size()) > fit) {
        char b[48];
        std::snprintf(b, sizeof b, "%d-%d of %zu", scroll + 1, std::min(scroll + fit, static_cast<int>(rows.size())), rows.size());
        cv.text(b, kWidth - 340, top + fit * kRowH + 30, 22, kDim, 300, 1);
    }
}

// The race list: row 0 creates (or leaves) a race, the others are the open races to join.
struct RaceRow {
    std::vector<std::string> cells;
    std::string join;  // room id to join; "" for row 0
    bool own = false;
};

std::vector<RaceRow> race_rows(const Data &d, const gt4::results::Room &cur) {
    std::vector<RaceRow> rows;
    if (cur.id.empty())
        rows.push_back({{"+  Create a race", "", "", "", "", "", ""}, "", false});
    else
        rows.push_back({{std::string("x  Leave race ") + cur.id, "", "", "", "", "", ""}, "", true});
    // Ping to the race = own round trip to the relay + the host's (all traffic goes through the relay).
    const int own_ping = gt4::results::pingMs();
    if (d.rooms.type == json::Value::Array)
        for (const json::Value &v : d.rooms.arr) {
            const std::string id = str(v.get("room"));
            if (id.empty()) continue;
            const long long n = num(v.get("count"), 1);
            // Who: the host first (the green Host column marks him), then the others.
            const std::string host = str(v.get("host"));
            std::string players = host;
            if (const json::Value *p = v.get("players"); p && p->type == json::Value::Array)
                for (const json::Value &name : p->arr)
                    if (str(&name) != host) players += ", " + str(&name);
            const long long max = num(v.get("max_players"));
            std::string state = str(v.get("state")) == "racing" ? "racing" : "waiting";
            if (state == "waiting" && max > 0 && n >= max) state = "full";
            std::string track = str(v.get("track"));
            if (track.empty()) track = "-";
            const std::string count = max > 0 ? std::to_string(n) + " / " + std::to_string(max) : std::to_string(n);
            const long long host_ping = num(v.get("host_ping"), -1);
            const std::string ping = own_ping >= 0 && host_ping >= 0 ? std::to_string(own_ping + host_ping) + " ms" : "-";
            rows.push_back({{host, id, track, count, players, ping, state}, id, id == cur.id});
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
    cv.text("Lobby", 330, 62, 24, kDim);
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
        if (const int ms = gt4::results::pingMs(); ms >= 0) {
            const std::size_t len = std::strlen(status);
            std::snprintf(status + len, sizeof status - len, "  ·  ping %d ms", ms);
        }
    }
    cv.text(status, kWidth - 440, 80, 20, d.fetched && !d.reached ? kAccent : kGreen, 400, 1);
    // How to connect: the three steps, the current one highlighted, then the race list.
    const gt4::results::Room cur = gt4::results::room();
    const std::vector<RaceRow> rr = race_rows(d, cur);
    g_sel = std::clamp(g_sel, 0, static_cast<int>(rr.size()) - 1);
    std::string host = "?";
    for (const RaceRow &row : rr)
        if (row.join == cur.id && !row.cells.empty()) host = row.cells[0];
    const int step = cur.id.empty() ? 1 : 2;
    cv.text("How to race online", 40, 146, 30, kText);
    struct Step {
        std::string text;
        int n;
    };
    const Step steps[] = {
        {"1   Choose  \"+ Create a race\"  to host one, or pick an open race below and press A to join it.", 1},
        {cur.id.empty() ? std::string("2   In GT4: Options > LAN Settings.  Host: Yes and No. of Players = seats (if you created the race),"
                                      "  Host: No (if you joined).")
         : cur.host     ? "2   You HOST race " + cur.id + ".  In GT4: Options > LAN Settings > Host: Yes, No. of Players = seats."
                        : "2   You joined race " + cur.id + " (host: " + host + ").  In GT4: Options > LAN Settings > Host: No.",
         2},
        {cur.id.empty() ? std::string("3   Then Arcade > Multi-LAN Race > Connect. The race starts when all of the host's seats are taken.")
         : cur.host     ? std::string("3   Then Arcade > Multi-LAN Race > Connect. The race starts when all seats are taken.")
                        : std::string("3   Then Arcade > Multi-LAN Race > Connect and wait for the host to start the race."),
         2},
    };
    for (int i = 0; i < 3; ++i) {
        const Color c = steps[i].n == step ? (cur.id.empty() ? kText : cur.host ? kGreen : kGold) : kDim;
        cv.text(steps[i].text, 56, 186 + i * 32, 23, c, kWidth - 110);
    }
    cv.text(cur.id.empty() ? "Without a race, Multi-LAN Race connects you with everyone in the open lobby."
                           : "Leave the race (first row) to go back to the open lobby.",
            56, 186 + 3 * 32, 21, kDim, kWidth - 110);
    std::vector<std::vector<std::string>> rows;
    std::vector<bool> mine;
    for (const RaceRow &row : rr) {
        rows.push_back(row.cells);
        mine.push_back(row.own);
    }
    constexpr int kRacesTop = 368;
    table(cv, {{"Host", 50, 170, 0}, {"Race", 230, 110, 0}, {"Track", 350, 290, 0}, {"Players", 650, 100, 0},
               {"Who", 760, 220, 0}, {"Ping", 990, 110, 1}, {"Status", 1110, 130, 1}},
          rows, mine, g_scroll, "", g_sel, kRacesTop, kGreen);
    if (rr.size() == 1) cv.text("No open races right now", 40, kRacesTop + kRowH + 60, 28, kDim, kWidth - 80, 2);
    // Footer
    cv.rect(0, kHeight - 56, kWidth, 56, kHeader);
    cv.text("Right stick  up/down: choose     A: select     B or both stick clicks: close", 40, kHeight - 20, 22, kDim,
            kWidth - 80, 2);
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
    (void)dx;  // one page: no tabs
    if (dy) {
        g_sel = std::max(0, g_sel + dy);
        g_dirty = true;
    }
}

void select() {
    std::string join;
    bool row0 = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
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

void setTab(int) {}  // one page

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

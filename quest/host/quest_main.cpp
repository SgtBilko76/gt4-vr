// Gran Turismo 4 (A-Spec II) on Meta Quest: the NativeActivity entry and the game's threads.
//
// Threads:
//   main  (android_main): Android lifecycle only.
//   xr    (quest_xr.cpp): OpenXR session, the game picture as a screen layer, Touch -> DualShock 2.
//   gs    : owns the PCSX2 GS (librrv-pcsx2-gs-bridge.so); renders with Vulkan straight into the
//           XR thread's Android-surface swapchain while draining the runtime's GS queue.
//   vu1   : the VU1 worker of ps2-web-recomp.
//   ee    : the recompiled game (256 MB stack), paced on the AAudio ring.
//
// Data on the headset: /sdcard/GT4VR/{core.elf, disc.iso, memcard.ps2, config.txt, log.txt}.
// disc.iso and memcard.ps2 are also taken from /sdcard/GT4 (the first GT4 Quest build) when absent.
// config.txt: KEY=VALUE lines; PS2_* and RRV_* keys become environment variables (runtime and
// PCSX2 GS switches), the rest are host settings.
#include "quest_host.h"
#include "quest_xr.h"

#include "ps2/system.hpp"
#include "../../runtime/src/gs/pcsx2_bridge.hpp"

#include <aaudio/AAudio.h>
#include <android/asset_manager.h>
#include <android/log.h>
#include <android_native_app_glue.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

void register_generated_code();

#include "gt4_camera.h"
#include <sys/system_properties.h>

namespace
{
// VR race view (gt4_camera.cpp): the head pose of the XR thread turns the race camera, unless the
// curved screen is chosen (both stick clicks) or debug.gt4vr.headtrack is 0.
gt4::camera::Head quest_head()
{
    static int countdown = 0;
    static bool enabled = true;
    if (countdown-- <= 0)
    {
        countdown = 60;
        char value[PROP_VALUE_MAX] = {};
        enabled = __system_property_get("debug.gt4vr.headtrack", value) <= 0 || std::atoi(value) != 0;
    }
    gt4::camera::Head out;
    if (!enabled || gt4::quest::curvedView())
        return out;
    const gt4::quest::HeadPose h = gt4::quest::currentHeadPose();
    out.qx = h.qx;
    out.qy = h.qy;
    out.qz = h.qz;
    out.qw = h.qw;
    out.px = h.px;
    out.py = h.py;
    out.pz = h.pz;
    out.key = h.key;
    out.tanX = h.renderTanX;
    out.tanY = h.renderTanY;
    return out;
}
} // namespace
void register_generated_vu();

namespace {

namespace fs = std::filesystem;

#ifndef GT4VR_DATA_DIR
#define GT4VR_DATA_DIR "/sdcard/GT4VR"
#define GT4VR_OLD_DATA_DIR "/sdcard/GT4"
#define GT4VR_TITLE "Gran Turismo 4 VR"
#define GT4VR_NET_PRODUCT "SCUS97436"
#define GT4VR_CAMERA_HOOKS 1
#endif
// The build's variant (CMakeLists.txt GT4VR_*): A-Spec II = /sdcard/GT4VR, retail v2.00 with LAN
// play = /sdcard/GT4LAN.
const std::string kDataDir = GT4VR_DATA_DIR;
const std::string kOldDataDir = GT4VR_OLD_DATA_DIR;

// ---------------------------------------------------------------- logging

FILE *g_log = nullptr;

void logf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    __android_log_print(ANDROID_LOG_INFO, "GT4VR", "%s", buf);
    std::fprintf(stderr, "%s\n", buf);
}

// Runtime and PCSX2 diagnostics go to stderr: into log.txt, and each line to logcat too.
void redirect_stderr() {
    int pipes[2];
    if (pipe(pipes) != 0) return;
    g_log = std::fopen((kDataDir + "/log.txt").c_str(), "w");
    dup2(pipes[1], STDERR_FILENO);
    close(pipes[1]);
    setvbuf(stderr, nullptr, _IOLBF, 0);
    std::thread([fd = pipes[0]] {
        std::string line;
        char chunk[512];
        for (ssize_t n; (n = read(fd, chunk, sizeof chunk)) > 0;) {
            for (ssize_t i = 0; i < n; ++i) {
                if (chunk[i] != '\n') {
                    line += chunk[i];
                    continue;
                }
                __android_log_write(ANDROID_LOG_INFO, "GT4VR-Game", line.c_str());
                if (g_log) {
                    std::fputs(line.c_str(), g_log);
                    std::fputc('\n', g_log);
                    std::fflush(g_log);
                }
                line.clear();
            }
        }
    }).detach();
}

// ---------------------------------------------------------------- config

std::map<std::string, std::string> g_config;

void load_config() {
    std::ifstream in(kDataDir + "/config.txt");
    std::string s;
    while (std::getline(in, s)) {
        while (!s.empty() && (s.back() == '\r' || s.back() == ' ')) s.pop_back();
        if (s.empty() || s[0] == '#') continue;
        const auto eq = s.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = s.substr(0, eq), value = s.substr(eq + 1);
        g_config[key] = value;
        if (key.rfind("PS2_", 0) == 0 || key.rfind("RRV_", 0) == 0) setenv(key.c_str(), value.c_str(), 1);
        logf("[quest] config %s=%s", key.c_str(), value.c_str());
    }
}

std::string config_string(const char *key, const std::string &fallback) {
    const auto it = g_config.find(key);
    return it == g_config.end() ? fallback : it->second;
}

bool readable(const std::string &path) { return access(path.c_str(), R_OK) == 0; }

// ---------------------------------------------------------------- APK assets -> files dir

// assets/gt4vr/manifest.txt lists every file (AAssetDir cannot list subdirectories); stamp.txt
// changes with the content, so the extraction only runs after an update.
std::string read_asset(AAssetManager *assets, const std::string &name) {
    AAsset *a = AAssetManager_open(assets, name.c_str(), AASSET_MODE_BUFFER);
    if (!a) return {};
    std::string data(static_cast<const char *>(AAsset_getBuffer(a)), static_cast<std::size_t>(AAsset_getLength(a)));
    AAsset_close(a);
    return data;
}

bool extract_assets(AAssetManager *assets, const fs::path &files) {
    const std::string stamp = read_asset(assets, "gt4vr/stamp.txt");
    if (stamp.empty()) return false;
    std::error_code ec;
    {
        std::ifstream have(files / "assets-stamp.txt");
        std::string old;
        std::getline(have, old);
        if (old == stamp && fs::exists(files / "resources", ec)) return true;
    }
    fs::remove_all(files / "resources", ec);
    std::istringstream manifest(read_asset(assets, "gt4vr/manifest.txt"));
    std::size_t count = 0;
    for (std::string rel; std::getline(manifest, rel);) {
        if (rel.empty()) continue;
        const std::string data = read_asset(assets, "gt4vr/" + rel);
        const fs::path out = files / rel;
        fs::create_directories(out.parent_path(), ec);
        std::ofstream(out, std::ios::binary).write(data.data(), static_cast<std::streamsize>(data.size()));
        ++count;
    }
    std::ofstream(files / "assets-stamp.txt") << stamp << "\n";
    logf("[quest] extracted %zu asset files (stamp %s)", count, stamp.c_str());
    return true;
}

// ---------------------------------------------------------------- audio (AAudio ring)

constexpr std::size_t kRingFrames = 48000;  // 1 s of stereo s16
std::vector<std::int16_t> g_ring(kRingFrames * 2);
std::atomic<std::size_t> g_ring_read{0}, g_ring_write{0};
AAudioStream *g_audio = nullptr;

std::size_t ring_queued() { return g_ring_write.load() - g_ring_read.load(); }

// Diagnostics for the perf line: loudest sample the game produced / the stream played, callbacks.
std::atomic<int> g_peak_in{0}, g_peak_out{0};
std::atomic<std::uint32_t> g_callbacks{0}, g_muted_callbacks{0};

aaudio_data_callback_result_t audio_callback(AAudioStream *, void *, void *data, int32_t frames) {
    auto *out = static_cast<std::int16_t *>(data);
    std::size_t r = g_ring_read.load();
    const std::size_t n = std::min<std::size_t>(g_ring_write.load() - r, static_cast<std::size_t>(frames));
    const bool muted = !gt4::quest::visible();
    int peak = 0;
    for (std::size_t i = 0; i < n; ++i, ++r) {
        const std::size_t at = (r % kRingFrames) * 2;
        out[i * 2] = muted ? 0 : g_ring[at];
        out[i * 2 + 1] = muted ? 0 : g_ring[at + 1];
        peak = std::max({peak, std::abs(static_cast<int>(out[i * 2])), std::abs(static_cast<int>(out[i * 2 + 1]))});
    }
    if (peak > g_peak_out.load(std::memory_order_relaxed)) g_peak_out.store(peak, std::memory_order_relaxed);
    g_callbacks.fetch_add(1, std::memory_order_relaxed);
    if (muted) g_muted_callbacks.fetch_add(1, std::memory_order_relaxed);
    std::memset(out + n * 2, 0, (static_cast<std::size_t>(frames) - n) * 4);
    g_ring_read.store(r);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void queue_audio(const std::int16_t *samples, std::size_t frames) {
    if (ring_queued() + frames > kRingFrames / 2) return;  // > 500 ms queued: drop
    int peak = g_peak_in.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < frames * 2; ++i) peak = std::max(peak, std::abs(static_cast<int>(samples[i])));
    g_peak_in.store(peak, std::memory_order_relaxed);
    std::size_t w = g_ring_write.load();
    for (std::size_t i = 0; i < frames; ++i, ++w) {
        const std::size_t at = (w % kRingFrames) * 2;
        g_ring[at] = samples[i * 2];
        g_ring[at + 1] = samples[i * 2 + 1];
    }
    g_ring_write.store(w);
}

void audio_error(AAudioStream *, void *, aaudio_result_t error);

void open_audio() {
    g_ring_read.store(g_ring_write.load());  // start empty (a reopen drops what the old stream left)
    AAudioStreamBuilder *builder = nullptr;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK) return;
    AAudioStreamBuilder_setSampleRate(builder, 48000);
    AAudioStreamBuilder_setChannelCount(builder, 2);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setDataCallback(builder, audio_callback, nullptr);
    AAudioStreamBuilder_setErrorCallback(builder, audio_error, nullptr);
    const aaudio_result_t r = AAudioStreamBuilder_openStream(builder, &g_audio);
    AAudioStreamBuilder_delete(builder);
    if (r != AAUDIO_OK) {
        logf("[quest] AAudio open failed: %s", AAudio_convertResultToText(r));
        g_audio = nullptr;
        return;
    }
    AAudioStream_requestStart(g_audio);
    logf("[quest] audio: AAudio 48000 Hz stereo, burst %d", AAudioStream_getFramesPerBurst(g_audio));
}

// Pace the game on the audio clock: wait while more than ~80 ms is queued. If the stream stops
// consuming (Horizon OS pauses or disconnects it, e.g. when the headset is taken off and put on),
// fall back to the frame clock and reopen the stream, instead of waiting 200 ms every field.
std::atomic<bool> g_audio_reopen{false};

void audio_error(AAudioStream *, void *, aaudio_result_t error) {
    std::fprintf(stderr, "[quest] audio stream error %s: reopening\n", AAudio_convertResultToText(error));
    g_audio_reopen = true;
}

// How much sound the game may queue ahead before it waits, in ms. Under heavy load the race drops
// below 60 fps for a few seconds and makes less sound than plays; the queue covers that dip, and the
// game refills it once the load passes. 80 ms ran dry in short dips (crackling, owner 2026-11-01);
// 200 ms covers ~10 s at 58 fps. config.txt RRV_AUDIO_BUFFER_MS=40..450 overrides.
std::size_t audio_target_frames() {
    static const std::size_t frames = [] {
        long ms = 200;
        if (const char *v = std::getenv("RRV_AUDIO_BUFFER_MS")) ms = std::clamp(std::atol(v), 40L, 450L);
        logf("[quest] audio buffer %ld ms", ms);
        return static_cast<std::size_t>(48 * ms);
    }();
    return frames;
}

void pace() {
    static auto last = std::chrono::steady_clock::now();
    static std::size_t last_read = 0;
    static int stalled = 0;
    if (g_audio_reopen.exchange(false)) {
        if (g_audio) AAudioStream_close(g_audio);
        g_audio = nullptr;
        open_audio();
    }
    if (g_audio) {
        const std::size_t read = g_ring_read.load();
        stalled = read == last_read ? stalled + 1 : 0;
        last_read = read;
        if (stalled < 30) {
            const std::size_t target = audio_target_frames();
            for (int guard = 0; ring_queued() > target && guard < 40; ++guard) usleep(1000);
            if (ring_queued() <= target) {
                last = std::chrono::steady_clock::now();
                return;
            }
        } else if (stalled == 30) {
            std::fprintf(stderr, "[quest] audio not consumed for 30 fields (state %d): frame clock, reopening\n",
                         static_cast<int>(AAudioStream_getState(g_audio)));
            g_audio_reopen = true;
        }
    }
    const auto period = std::chrono::microseconds(16683);
    const auto now = std::chrono::steady_clock::now();
    if (now - last < period) {
        std::this_thread::sleep_for(period - (now - last));
        last += period;
    } else {
        last = now;
    }
}

// ---------------------------------------------------------------- pad (EE thread, once a field)

std::uint8_t stick_byte(float v, bool invert) {
    if (std::fabs(v) < 0.08f) v = 0.0f;
    if (invert) v = -v;
    return static_cast<std::uint8_t>(std::clamp(128.0f + v * 127.5f, 0.0f, 255.0f));
}

std::uint8_t pressure_byte(float v) { return v > 0.0f ? static_cast<std::uint8_t>(std::clamp(v, 0.0f, 1.0f) * 254.0f + 1.0f) : 0; }

void apply_pad() {
    const gt4::quest::Pad pad = gt4::quest::currentPad();
    const std::uint8_t lx = stick_byte(pad.leftX, false), ly = stick_byte(pad.leftY, true);
    const std::uint8_t cross = pressure_byte(pad.cross), square = pressure_byte(pad.square);
    static std::uint64_t last = ~0ull;
    const std::uint64_t state = (pad.buttons & 0xFFFFu) | (std::uint64_t(lx) << 16) | (std::uint64_t(ly) << 24) | (std::uint64_t(cross) << 32) |
                                (std::uint64_t(square) << 40);
    const std::uint16_t motors = ps2::pad_rumble(0);
    const double strong = (motors >> 8) < 16 ? 0.0 : std::sqrt((motors >> 8) / 255.0);
    const bool weak = motors & 1u;
    gt4::quest::setRumble(static_cast<float>(strong), static_cast<float>(weak ? 1.0 : 0.0));
    if (state == last) return;
    last = state;
    ps2::set_pad_pressure(14, cross ? cross : 255, 0);
    ps2::set_pad_pressure(15, square ? square : 255, 0);
    ps2::set_pad_buttons(static_cast<std::uint16_t>(pad.buttons & 0xFFFFu), 0);
    ps2::set_pad_axis(0, 128, 0);  // right stick stays centred: it drives the d-pad
    ps2::set_pad_axis(1, 128, 0);
    ps2::set_pad_axis(2, lx, 0);
    ps2::set_pad_axis(3, ly, 0);
}

// ---------------------------------------------------------------- game threads

std::atomic<bool> g_game_done{false};
std::atomic<int> g_gs_state{0};  // 0 starting, 1 ready, -1 failed
std::string g_elf_path;

void *ee_thread_main(void *) {
    pthread_setname_np(pthread_self(), "gt4-ee");
    std::uint32_t entry = 0;
    if (!ps2::load_elf(g_elf_path, entry)) {
        logf("[quest] cannot load %s", g_elf_path.c_str());
        g_game_done = true;
        return nullptr;
    }
    ps2::boot(entry, {g_elf_path});
    const std::string state = config_string("load_state", "");
    if (!state.empty()) logf("[quest] load state %s: %s", state.c_str(), ps2::load_state(state) ? "ok" : "FAILED");
    logf("[quest] game running");
    const int result = ps2::run();
    logf("[quest] game stopped (%d): %s", result, ps2::halt_reason().c_str());
    g_game_done = true;
    ps2::wake_render_thread();
    gt4::quest::requestClose();
    return nullptr;
}

// Owns the PCSX2 GS. It must be up, and the GS queue running, before the EE starts, or the first
// GS work is lost.
void gs_thread_main() {
    pthread_setname_np(pthread_self(), "gt4-gs");
    gt4::quest::Surface surface;
    while (!gt4::quest::waitForSurface(&surface, 500))
        if (gt4::quest::closeRequested()) {
            g_gs_state = -1;
            return;
        }
    std::string error;
    if (!ps2::gs::bridge::open(surface.window, surface.width, surface.height, error)) {
        logf("[quest] PCSX2 GS failed: %s", error.c_str());
        g_gs_state = -1;
        return;
    }
    ps2::init_graphics();
    ps2::start_render_thread();
    g_gs_state = 1;
    while (!g_game_done.load() && !gt4::quest::closeRequested()) {
        if (ps2::render_pending(false, nullptr, nullptr, 8.0) == ps2::Rendered::idle) ps2::wait_render_work(4);
    }
}

bool start_game(const fs::path &files) {
    fs::create_directories(kDataDir);
    g_elf_path = kDataDir + "/core.elf";
    std::string disc = kDataDir + "/disc.iso";
    if (!readable(disc) && !kOldDataDir.empty() && readable(kOldDataDir + "/disc.iso")) disc = kOldDataDir + "/disc.iso";
    const std::string card = kDataDir + "/memcard.ps2";
    if (!readable(card) && !kOldDataDir.empty() && readable(kOldDataDir + "/memcard.ps2")) {
        std::error_code ec;
        fs::copy_file(kOldDataDir + "/memcard.ps2", card, ec);
        logf("[quest] memory card copied from %s: %s", kOldDataDir.c_str(), ec ? ec.message().c_str() : "ok");
    }
    if (!readable(g_elf_path) || !readable(disc)) {
        logf("[quest] missing %s or %s", g_elf_path.c_str(), disc.c_str());
        return false;
    }
    logf("[quest] elf %s, disc %s", g_elf_path.c_str(), disc.c_str());

    // PCSX2 GS: resources from the APK, its caches in the app's files dir; the XR host decides the
    // screen's shape, so PCSX2 fills the surface.
    setenv("RRV_PCSX2_GS_RESOURCE_DIR", (files / "resources").c_str(), 0);
    setenv("RRV_PCSX2_GS_DATA_DIR", (files / "pcsx2-data").c_str(), 0);
    setenv("RRV_PCSX2_GS_ASPECT", "stretch", 0);
    // GT4 profile of the bridge (PCSX2 GameDB fixes for GT4, field-mode upscaling); scale 2 =
    // scale 3 = 1920x1344 for the 640x448 picture (owner choice 2026-10-30: race 60 fps, a few
    // skipped fields in heavy scenes). config.txt RRV_PCSX2_GS_SCALE=N overrides.
    setenv("RRV_PCSX2_GS_GAME", "gt4", 0);
    // Anisotropic: 2.5x wide, 4x high (1600x1792). VR lacks vertical detail most (448 lines for the
    // whole view); measured 2026-10-30 at the same cost as 3x3 (VR 72 Hz: 108-123 of 600 skipped vs
    // 100-122), 3x4 skipped 139-159, 4x4 190-240. RRV_PCSX2_GS_SCALE_X=3 RRV_PCSX2_GS_SCALE=3 = old.
    // VR mode re-enabled (owner: VR and curved screen switchable); the curved screen stays the
    // default until the owner toggles back. For VR later: RRV_PCSX2_GS_SCALE_X=2.5 RRV_PCSX2_GS_SCALE=4.
    setenv("RRV_PCSX2_GS_SCALE", "3", 0);
    // Sharpness (owner 2026-10-30: "das Bild ist total unscharf"): AMD CAS on the presented frame
    // and 8x anisotropic texture filtering (far road and walls). config.txt overrides
    // (RRV_PCSX2_GS_CAS=0 / RRV_PCSX2_GS_ANISO=0 = off).
    // Owner 2026-11-01 at 3x3 after the eye-sized CAS fix: 60 too soft, 70/80 too sharp -> 50.
    setenv("RRV_PCSX2_GS_CAS", "50", 0);
    setenv("RRV_PCSX2_GS_ANISO", "8", 0);
    // Anti-aliasing (owner 2026-10-30: "3 times PS2 resolution and antialiasing, that works good at
    // Wipeout"): PCSX2's hardware renderer has no MSAA, FXAA on the presented frame is its edge
    // smoothing. RRV_PCSX2_GS_FXAA=0 in config.txt turns it off.
    // Measured 2026-10-30 (VR, 72 Hz): FXAA cost ~25 of 600 fields skipped for little visible gain;
    // owner chose it off. RRV_PCSX2_GS_FXAA=1 turns it back on.
    // Owner now: picture unsharp again -> turn anti-aliasing back on to optimise the sharpness.
    setenv("RRV_PCSX2_GS_FXAA", "1", 0);
    // Flicker with a light source in view (owner 2026-11-01 video): the bridge's field skipping
    // dropped whole fields when the GS fell behind, GT4's brightness / glare post-processing passes
    // included, so the picture pulsed 3-4 times a second. Off, the flicker is gone (owner tested);
    // the eye-sized CAS keeps the race at 60 fps without it. RRV_PCSX2_GS_FRAMESKIP=1 = old.
    setenv("RRV_PCSX2_GS_FRAMESKIP", "0", 0);
    // Hardware download Unsynchronized (ARMSX2 Quest's choice; owner 2026-11-01) instead of the
    // bridge profile's Disabled. RRV_PCSX2_GS_HWDOWNLOAD=4 = Disabled.
    setenv("RRV_PCSX2_GS_HWDOWNLOAD", "3", 0);
    // Shader pipelines (owner 2026-11-01: collect / precompile the shaders). The bridge records every
    // pipeline it builds in its cache and builds the whole list up front at start; the list is
    // mirrored to /sdcard/GT4VR (the app is not debuggable) so quest/tools/quest_pull_pipelines.sh
    // can take it into quest/assets, and build_quest.sh ships it as the seed for fresh installs.
    setenv("RRV_PIPELINE_EXPORT", (kDataDir + "/rrv_tfx_pipelines.bin").c_str(), 0);
    if (const fs::path seed = files / "pipelines" / "rrv_tfx_pipelines.bin"; fs::exists(seed))
        setenv("RRV_PIPELINE_SEED", seed.c_str(), 0);
    fs::create_directories(files / "pcsx2-data");

    ps2::g_options.disc_path = disc;
    ps2::g_options.memcard_path = card;
    ps2::g_options.pal = false;  // A-Spec II and the retail USA v2.00 are NTSC
    // Network play (runtime/src/net/link.hpp): config.txt PS2_NET_SERVER=host turns the PS2 network
    // adapter on; players of the same product code and group meet on that ad hoc relay server.
    setenv("PS2_NET_PRODUCT", GT4VR_NET_PRODUCT, 0);
    ps2::g_options.rate_forced = true;
    ps2::g_options.widescreen = config_string("widescreen", "1") == "1";
    ps2::g_options.on_vblank = [] {
        ps2::post_present();
        apply_pad();
        pace();
    };
    ps2::set_audio_sink(queue_audio);
    register_generated_code();
    register_generated_vu();
    if (GT4VR_CAMERA_HOOKS) gt4::camera::install(quest_head, gt4::quest::noteHeadTrackedFrame);
    ps2::gs::bridge::set_field_tags(gt4::quest::takeBuiltKey, gt4::quest::notePresentedKey);

    std::thread(gs_thread_main).detach();
    while (g_gs_state.load() == 0) usleep(2000);
    if (g_gs_state.load() < 0) return false;
    ps2::start_vu_thread();
    std::thread([] {
        pthread_setname_np(pthread_self(), "gt4-vu1");
        ps2::run_vu_thread();
    }).detach();

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256u << 20);
    pthread_t ee;
    const int rc = pthread_create(&ee, &attr, ee_thread_main, nullptr);
    pthread_attr_destroy(&attr);
    if (rc) {
        logf("[quest] pthread_create(ee) failed: %d", rc);
        return false;
    }
    pthread_detach(ee);
    return true;
}

void perf_loop() {
    pthread_setname_np(pthread_self(), "gt4-perf");
    std::uint64_t last_frames = ps2::frames_run(), last_fields = ps2::gs::bridge::fields(), last_interp = ps2::vu1_interpreted_calls();
    auto last = std::chrono::steady_clock::now();
    while (!gt4::quest::closeRequested()) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        const auto now = std::chrono::steady_clock::now();
        const double s = std::chrono::duration<double>(now - last).count();
        const std::uint64_t frames = ps2::frames_run(), fields = ps2::gs::bridge::fields(), interp = ps2::vu1_interpreted_calls();
        logf("[perf] game %.1f fps, GS fields %.1f/s, audio queued %zu ms, vu1 interpreted %.0f/s | audio peak game %d played %d, "
             "callbacks %u (%u muted), stream state %d",
             (frames - last_frames) / s, (fields - last_fields) / s, ring_queued() / 48, (interp - last_interp) / s, g_peak_in.exchange(0),
             g_peak_out.exchange(0), g_callbacks.exchange(0), g_muted_callbacks.exchange(0),
             g_audio ? static_cast<int>(AAudioStream_getState(g_audio)) : -1);
        last_frames = frames;
        last_fields = fields;
        last_interp = interp;
        last = now;
    }
}

void on_app_cmd(android_app *, int32_t) {}

}  // namespace

void android_main(android_app *app) {
    app->onAppCmd = on_app_cmd;
    mkdir(kDataDir.c_str(), 0775);
    redirect_stderr();
    logf("[quest] %s starting (data %s)", GT4VR_TITLE, kDataDir.c_str());
    load_config();

    const fs::path files = app->activity->internalDataPath;
    if (!extract_assets(app->activity->assetManager, files)) logf("[quest] no PCSX2 resources in the APK");

    JavaVM *vm = app->activity->vm;
    jobject activity = app->activity->clazz;
    std::thread xr([vm, activity] { gt4::quest::runXrThread(vm, activity); });

    open_audio();
    // The game starts on its own thread: start_game waits for the XR surface, which needs the
    // session to become READY, which needs this thread's looper to answer the activity's
    // lifecycle (native_app_glue blocks onResume until APP_CMD_RESUME is processed here).
    std::thread([files] {
        if (!start_game(files)) logf("[quest] game not started: copy core.elf and disc.iso to %s", kDataDir.c_str());
        else perf_loop();
    }).detach();

    while (!app->destroyRequested) {
        int events = 0;
        android_poll_source *source = nullptr;
        if (ALooper_pollOnce(100, nullptr, &events, reinterpret_cast<void **>(&source)) >= 0 && source) source->process(app, source);
        if (gt4::quest::closeRequested() && g_game_done.load()) ANativeActivity_finish(app->activity);
    }
    gt4::quest::requestClose();
    xr.join();
    logf("[quest] activity destroyed");
    // NativeActivity reuses the process for the next launch; the game's globals are not re-entrant.
    _exit(0);
}

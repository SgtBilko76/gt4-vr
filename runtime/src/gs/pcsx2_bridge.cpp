#include "pcsx2_bridge.hpp"

#include "rrv_pcsx2_gs_bridge.h"

#include <dlfcn.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ps2::gs::bridge {

namespace {

constexpr std::uint32_t kErrorCapacity = 512;

using VersionFn = decltype(&rrv_pcsx2_gs_bridge_version);
using CreateFn = decltype(&rrv_pcsx2_gs_bridge_create);
using DestroyFn = decltype(&rrv_pcsx2_gs_bridge_destroy);
using SubmitFn = decltype(&rrv_pcsx2_gs_bridge_submit);
using VsyncFn = decltype(&rrv_pcsx2_gs_bridge_vsync);
using ReadLocalFn = decltype(&rrv_pcsx2_gs_bridge_read_local_memory);
using ResetFn = decltype(&rrv_pcsx2_gs_bridge_reset_gs);
using SnapshotMemFn = decltype(&rrv_pcsx2_gs_bridge_snapshot_local_memory);
using RestoreMemFn = decltype(&rrv_pcsx2_gs_bridge_restore_local_memory);

struct Library {
    void *handle = nullptr;
    VersionFn version = nullptr;
    CreateFn create = nullptr;
    DestroyFn destroy = nullptr;
    SubmitFn submit = nullptr;
    VsyncFn vsync = nullptr;
    ReadLocalFn read_local = nullptr;
    ResetFn reset = nullptr;  // optional
    SnapshotMemFn snapshot_mem = nullptr;
    RestoreMemFn restore_mem = nullptr;
} g_lib;

RrvPcsx2GsBridge *g_bridge = nullptr;
std::atomic<bool> g_active{false};
std::atomic<std::uint64_t> g_fields{0};
std::uint64_t g_field_index = 0;
char g_error[kErrorCapacity];
unsigned g_failures = 0;

void report(const char *what) {
    // A failing call usually fails every frame after; the first few say why.
    if (++g_failures <= 8) std::fprintf(stderr, "[pcsx2] %s failed: %s\n", what, g_error);
    g_error[0] = 0;
}

template <typename Fn>
bool resolve(Fn &out, const char *name, std::string &error) {
    out = reinterpret_cast<Fn>(dlsym(g_lib.handle, name));
    if (!out) error = std::string("bridge has no ") + name;
    return out != nullptr;
}

std::uint32_t render_mode() {
    const char *mode = std::getenv("PS2_PCSX2_RENDER_MODE");
    return mode && std::strcmp(mode, "full") == 0 ? RRV_PCSX2_GS_RENDER_MODE_FULL_FRAME : RRV_PCSX2_GS_RENDER_MODE_FIELD;
}

}  // namespace

bool active() { return g_active.load(std::memory_order_acquire); }

bool open(void *window, std::uint32_t width, std::uint32_t height, std::string &error) {
    const char *path = std::getenv("PS2_PCSX2_BRIDGE");
    if (!path || !*path) path = "librrv-pcsx2-gs-bridge.so";
    g_lib.handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!g_lib.handle) {
        const char *why = dlerror();
        error = std::string("dlopen ") + path + ": " + (why ? why : "?");
        return false;
    }
    if (!resolve(g_lib.version, "rrv_pcsx2_gs_bridge_version", error) || !resolve(g_lib.create, "rrv_pcsx2_gs_bridge_create", error) ||
        !resolve(g_lib.destroy, "rrv_pcsx2_gs_bridge_destroy", error) || !resolve(g_lib.submit, "rrv_pcsx2_gs_bridge_submit", error) ||
        !resolve(g_lib.vsync, "rrv_pcsx2_gs_bridge_vsync", error) ||
        !resolve(g_lib.read_local, "rrv_pcsx2_gs_bridge_read_local_memory", error))
        return false;
    g_lib.reset = reinterpret_cast<ResetFn>(dlsym(g_lib.handle, "rrv_pcsx2_gs_bridge_reset_gs"));
    g_lib.snapshot_mem = reinterpret_cast<SnapshotMemFn>(dlsym(g_lib.handle, "rrv_pcsx2_gs_bridge_snapshot_local_memory"));
    g_lib.restore_mem = reinterpret_cast<RestoreMemFn>(dlsym(g_lib.handle, "rrv_pcsx2_gs_bridge_restore_local_memory"));
    if (g_lib.version() != RRV_PCSX2_GS_BRIDGE_ABI_VERSION) {
        error = "bridge ABI " + std::to_string(g_lib.version()) + ", expected " + std::to_string(RRV_PCSX2_GS_BRIDGE_ABI_VERSION);
        return false;
    }

    RrvPcsx2GsBridgeSurface surface{};
    surface.struct_size = sizeof surface;
    surface.backing_scale = 1.0f;
    const bool direct = window != nullptr;
    if (direct) {
#if defined(__ANDROID__)
        surface.kind = RRV_PCSX2_GS_SURFACE_ANDROID;
#else
        surface.kind = RRV_PCSX2_GS_SURFACE_LINUX_X11;
#endif
        surface.flags = RRV_PCSX2_GS_SURFACE_FLAG_CALLER_OWNS_HANDLES;
        // The runtime side of the RRV port learnt that a null layer reads as "no surface";
        // the Android adapter accepts the window in both slots.
        surface.native_view = window;
        surface.native_layer = window;
        surface.width_pixels = width;
        surface.height_pixels = height;
    }
    RrvPcsx2GsBridgeConfig config{};
    config.struct_size = sizeof config;
    config.gs_render_mode = render_mode();
    config.presentation_mode = direct ? RRV_PCSX2_GS_PRESENTATION_DIRECT_GPU : RRV_PCSX2_GS_PRESENTATION_LEGACY_CPU_SNAPSHOT;
#if defined(__ANDROID__)
    config.renderer_kind = RRV_PCSX2_GS_RENDERER_VULKAN;
#else
    config.renderer_kind = direct ? RRV_PCSX2_GS_RENDERER_AUTO : RRV_PCSX2_GS_RENDERER_SOFTWARE;
#endif
    config.surface = surface;
    RrvPcsx2GsBridgeCapabilities caps{};
    caps.struct_size = sizeof caps;
    g_bridge = g_lib.create(&config, &caps, g_error, kErrorCapacity);
    if (!g_bridge) {
        error = std::string("create: ") + g_error;
        return false;
    }
    std::fprintf(stderr, "[pcsx2] GS bridge up: %s, %s mode, %ux%u, capabilities 0x%X\n", caps.renderer_name ? caps.renderer_name : "?",
                 config.gs_render_mode == RRV_PCSX2_GS_RENDER_MODE_FULL_FRAME ? "full-frame" : "field", width, height, caps.capability_flags);
    g_active.store(true, std::memory_order_release);
    return true;
}

void close() {
    g_active.store(false, std::memory_order_release);
    if (g_bridge) g_lib.destroy(g_bridge);
    g_bridge = nullptr;
}

void submit(int path, const std::uint8_t *data, std::uint32_t bytes) {
    if (!g_lib.submit(g_bridge, static_cast<std::uint32_t>(path), data, bytes, g_error, kErrorCapacity)) report("submit");
}

void vsync(const Field &field) {
    if (!g_lib.vsync(g_bridge, field.regs, g_field_index++, field.parity & 1u, g_error, kErrorCapacity)) report("vsync");
    g_fields.fetch_add(1, std::memory_order_relaxed);
}

void download(const Download &request, std::uint32_t bytes) {
    auto *dst = reinterpret_cast<std::uint8_t *>(request.target);
    if (!g_lib.read_local(g_bridge, dst, bytes, request.bitbltbuf, request.trxpos, request.trxreg, g_error, kErrorCapacity)) {
        report("read_local_memory");
        std::memset(dst, 0, bytes);
    }
}

void reset() {
    if (g_lib.reset && !g_lib.reset(g_bridge, g_error, kErrorCapacity)) report("reset_gs");
}

bool snapshot_memory(std::uint8_t *dst, std::uint32_t bytes) {
    if (!g_lib.snapshot_mem || !g_lib.snapshot_mem(g_bridge, dst, bytes, g_error, kErrorCapacity)) {
        report("snapshot_local_memory");
        return false;
    }
    return true;
}

bool restore_memory(const std::uint8_t *src, std::uint32_t bytes) {
    if (!g_lib.restore_mem || !g_lib.restore_mem(g_bridge, src, bytes, g_error, kErrorCapacity)) {
        report("restore_local_memory");
        return false;
    }
    return true;
}

std::uint64_t fields() { return g_fields.load(std::memory_order_relaxed); }

}

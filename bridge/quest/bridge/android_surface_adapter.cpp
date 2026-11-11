// SPDX-License-Identifier: GPL-3.0-or-later
//
// Android (Meta Quest) counterpart of tools/pcsx2-gs-bridge/linux_surface_adapter.cpp.
//
// Surface contract (RRV_PCSX2_GS_SURFACE_ANDROID, rrv_pcsx2_gs_bridge.h):
//   native_view  = ANativeWindow*  -- ANativeWindow_fromSurface(env, surface) of
//                  * a SurfaceView's Surface (2D panel app), or
//                  * the jobject Surface that OpenXR's xrCreateSwapchainAndroidSurfaceKHR
//                    (XR_KHR_android_surface_swapchain) returns, for an immersive app that
//                    shows the bridge's output on a quad/cylinder composition layer.
//   native_layer = NULL (reserved; the same pointer as native_view is accepted).
//   width_pixels/height_pixels = the window's buffer size, backing_scale > 0.
//   flags must include RRV_PCSX2_GS_SURFACE_FLAG_CALLER_OWNS_HANDLES.
// The caller keeps its own reference until rrv_pcsx2_gs_bridge_destroy() returns; the
// bridge additionally holds one (ANativeWindow_acquire in configure, _release in clear),
// so a Java-side release racing the GS thread cannot free the window under PCSX2's
// VkSurfaceKHR (the failure ARMSX2's native-lib.cpp documents for its own surface).
// PCSX2's Vulkan device then does vkCreateAndroidSurfaceKHR + swap chain on it
// (VKSwapChain::CreateVulkanSurface, WindowInfo::Type::Android), exactly as in ARMSX2.
//
// Like X11/Wayland, Android WSI has no main-thread rule: the owner-thread sideband
// (prepare -> consume) keeps the macOS/Linux contract and pump_main_thread is a no-op.
//
// Paths: rrv_pcsx2_gs_bridge_android_set_paths() (or the environment) say where the
// PCSX2 resources were extracted and which writable directory PCSX2 may use; bridge.cpp
// asks rrv_quest_bridge_resource_directory()/rrv_quest_bridge_apply_data_directory().

#include "rrv_pcsx2_gs_bridge.h"

#include <android/native_window.h>
#include <dlfcn.h>
#include <sys/stat.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>

#define RRV_QUEST_EXPORT extern "C" __attribute__((visibility("default")))
#define RRV_QUEST_INTERNAL extern "C" __attribute__((visibility("hidden")))

namespace
{
std::mutex s_preparation_mutex;
std::optional<RrvPcsx2GsBridgeSurface> s_prepared_surface;

std::mutex s_paths_mutex;
std::string s_resource_dir; // set by rrv_pcsx2_gs_bridge_android_set_paths
std::string s_data_dir;
std::string s_resolved_resource_dir; // what rrv_quest_bridge_resource_directory returned last

void SetError(char* destination, uint32_t capacity, const char* format, ...)
{
    if (!destination || capacity == 0u)
        return;
    va_list args;
    va_start(args, format);
    std::vsnprintf(destination, capacity, format, args);
    va_end(args);
    destination[capacity - 1u] = '\0';
}

bool IsDirectory(const std::string& path)
{
    struct stat st{};
    return !path.empty() && ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string EnvString(const char* name)
{
    const char* value = std::getenv(name);
    return (value && value[0] != '\0') ? std::string(value) : std::string();
}

std::string LibraryDirectory()
{
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(&LibraryDirectory), &info) == 0 || !info.dli_fname)
        return {};
    std::string path(info.dli_fname);
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string StripTrailingSlash(std::string path)
{
    while (path.size() > 1 && path.back() == '/')
        path.pop_back();
    return path;
}
} // namespace

// ---------------------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------------------

// Call before rrv_pcsx2_gs_bridge_create(). resource_dir: the directory holding PCSX2's
// resources (shaders/, fonts/, GameIndex.yaml, ...: <build>/pcsx2-gs-bridge/resources,
// shipped as APK assets and copied to e.g. getFilesDir()/pcsx2-resources). data_dir: a
// writable directory (e.g. getFilesDir()/pcsx2-data) where PCSX2 keeps its pipeline/shader
// cache and settings (it creates <data_dir>/ARMSX2/...). Either may be NULL to keep the
// previous value. Returns 0 when a given directory does not exist.
RRV_QUEST_EXPORT int rrv_pcsx2_gs_bridge_android_set_paths(const char* resource_dir, const char* data_dir)
{
    std::lock_guard<std::mutex> lock(s_paths_mutex);
    int ok = 1;
    if (resource_dir)
    {
        const std::string dir = StripTrailingSlash(resource_dir);
        if (IsDirectory(dir))
            s_resource_dir = dir;
        else
            ok = 0;
    }
    if (data_dir)
    {
        const std::string dir = StripTrailingSlash(data_dir);
        if (IsDirectory(dir) || ::mkdir(dir.c_str(), 0700) == 0)
            s_data_dir = dir;
        else
            ok = 0;
    }
    return ok;
}

// Resource directory, in order: rrv_pcsx2_gs_bridge_android_set_paths(),
// $RRV_PCSX2_GS_RESOURCE_DIR, <directory of this library>/resources (the desktop layout;
// only useful for an adb-pushed bridge outside an APK). Empty when none exists.
RRV_QUEST_INTERNAL const char* rrv_quest_bridge_resource_directory()
{
    std::lock_guard<std::mutex> lock(s_paths_mutex);
    std::string candidate = s_resource_dir;
    if (!IsDirectory(candidate))
        candidate = StripTrailingSlash(EnvString("RRV_PCSX2_GS_RESOURCE_DIR"));
    if (!IsDirectory(candidate))
    {
        const std::string lib = LibraryDirectory();
        candidate = lib.empty() ? std::string() : lib + "/resources";
    }
    s_resolved_resource_dir = IsDirectory(candidate) ? candidate : std::string();
    return s_resolved_resource_dir.c_str();
}

// PCSX2 (EmuFolders::SetDataDirectory, __linux__ branch, which Android takes) puts its data
// root at $XDG_CONFIG_HOME/ARMSX2, else $HOME/.config/ARMSX2 -- neither is set to anything
// writable in an app process. Point XDG_CONFIG_HOME at the data directory, in order:
// rrv_pcsx2_gs_bridge_android_set_paths(), $RRV_PCSX2_GS_DATA_DIR, <resources>/../pcsx2-data.
// An XDG_CONFIG_HOME the app set itself is left alone when neither of the first two is given.
RRV_QUEST_INTERNAL void rrv_quest_bridge_apply_data_directory()
{
    std::string dir;
    {
        std::lock_guard<std::mutex> lock(s_paths_mutex);
        dir = s_data_dir;
        if (dir.empty())
            dir = StripTrailingSlash(EnvString("RRV_PCSX2_GS_DATA_DIR"));
        if (dir.empty() && !EnvString("XDG_CONFIG_HOME").empty())
            return;
        if (dir.empty() && !s_resolved_resource_dir.empty())
        {
            const size_t slash = s_resolved_resource_dir.rfind('/');
            if (slash != std::string::npos && slash > 0)
                dir = s_resolved_resource_dir.substr(0, slash) + "/pcsx2-data";
        }
    }
    if (dir.empty())
        return;
    if (!IsDirectory(dir))
        ::mkdir(dir.c_str(), 0700);
    if (IsDirectory(dir))
        ::setenv("XDG_CONFIG_HOME", dir.c_str(), 1);
}

// ---------------------------------------------------------------------------------------
// Window references (gsrunner_host.cpp)
// ---------------------------------------------------------------------------------------

RRV_QUEST_INTERNAL void rrv_quest_bridge_retain_window(void* window)
{
    if (window)
        ANativeWindow_acquire(static_cast<ANativeWindow*>(window));
}

RRV_QUEST_INTERNAL void rrv_quest_bridge_release_window(void* window)
{
    if (window)
        ANativeWindow_release(static_cast<ANativeWindow*>(window));
}

// ---------------------------------------------------------------------------------------
// Surface checks + owner-thread sideband (same contract as linux_surface_adapter.cpp)
// ---------------------------------------------------------------------------------------

RRV_QUEST_INTERNAL int rrv_pcsx2_gs_bridge_validate_android_surface(
    const RrvPcsx2GsBridgeSurface* surface, char* error, uint32_t error_capacity)
{
    if (!surface || surface->struct_size < sizeof(*surface))
    {
        SetError(error, error_capacity, "direct presentation surface is missing or too small");
        return 0;
    }
    if (surface->kind != RRV_PCSX2_GS_SURFACE_ANDROID)
    {
        SetError(error, error_capacity, "direct presentation surface kind %u is not Android (%u)",
                 surface->kind, static_cast<unsigned>(RRV_PCSX2_GS_SURFACE_ANDROID));
        return 0;
    }
    if ((surface->flags & RRV_PCSX2_GS_SURFACE_FLAG_CALLER_OWNS_HANDLES) == 0u)
    {
        SetError(error, error_capacity, "direct presentation surface handles must be caller-owned");
        return 0;
    }
    if (!surface->native_view)
    {
        SetError(error, error_capacity, "direct presentation native_view is not an ANativeWindow*");
        return 0;
    }
    if (surface->native_layer && surface->native_layer != surface->native_view)
    {
        SetError(error, error_capacity, "direct presentation native_layer must be NULL for the Android kind");
        return 0;
    }
    if (surface->width_pixels == 0u || surface->height_pixels == 0u || !(surface->backing_scale > 0.0f))
    {
        SetError(error, error_capacity, "direct presentation surface has an empty extent or scale");
        return 0;
    }
    // A window whose Surface was released/abandoned on the Java side answers queries with
    // a negative status instead of a size; refuse it here rather than in the Vulkan loader.
    ANativeWindow* window = static_cast<ANativeWindow*>(surface->native_view);
    const int32_t width = ANativeWindow_getWidth(window);
    const int32_t height = ANativeWindow_getHeight(window);
    if (width < 0 || height < 0)
    {
        SetError(error, error_capacity, "direct presentation ANativeWindow is no longer valid (%d x %d)",
                 static_cast<int>(width), static_cast<int>(height));
        return 0;
    }
    return 1;
}

RRV_QUEST_EXPORT int rrv_pcsx2_gs_bridge_prepare_owner_surface(
    RrvPcsx2GsBridgeSurface* surface, char* error, uint32_t error_capacity)
{
    if (!rrv_pcsx2_gs_bridge_validate_android_surface(surface, error, error_capacity))
        return 0;
    surface->flags |= RRV_PCSX2_GS_SURFACE_FLAG_OWNER_THREAD;
    std::lock_guard<std::mutex> lock(s_preparation_mutex);
    s_prepared_surface = *surface;
    return 1;
}

RRV_QUEST_INTERNAL int rrv_pcsx2_gs_bridge_consume_owner_surface(
    const RrvPcsx2GsBridgeSurface* surface, char* error, uint32_t error_capacity)
{
    std::lock_guard<std::mutex> lock(s_preparation_mutex);
    if (!s_prepared_surface || !surface ||
        s_prepared_surface->kind != surface->kind ||
        s_prepared_surface->native_view != surface->native_view ||
        s_prepared_surface->native_layer != surface->native_layer ||
        s_prepared_surface->width_pixels != surface->width_pixels ||
        s_prepared_surface->height_pixels != surface->height_pixels ||
        s_prepared_surface->backing_scale != surface->backing_scale)
    {
        SetError(error, error_capacity, "GS owner surface has no matching preparation");
        return 0;
    }
    s_prepared_surface.reset();
    return 1;
}

RRV_QUEST_EXPORT void rrv_pcsx2_gs_bridge_owner_command(RrvPcsx2GsBridgeOwnerCommand command, void* context)
{
    if (command)
        command(context);
}

// PCSX2's Vulkan device never dispatches attach/detach to a platform main thread on
// Android either. Returns 1 ("serviced") as on Linux.
RRV_QUEST_EXPORT int rrv_pcsx2_gs_bridge_pump_main_thread()
{
    return 1;
}

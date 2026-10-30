// SPDX-License-Identifier: GPL-3.0-or-later
#include "quest_host.h"

#include <android/native_window.h>
#include <sys/system_properties.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <utility>

namespace gt4::quest
{
namespace
{
std::mutex s_surfaceMutex;
std::condition_variable s_surfaceChanged;
Surface s_surface;

std::mutex s_padMutex;
Pad s_pad;

std::mutex s_menuMutex;
bool s_menuToggle = false;
std::deque<std::pair<uint32_t, bool>> s_menuKeys;
std::atomic<bool> s_menuOpen{false};

std::atomic<float> s_rumbleLarge{0.0f};
std::atomic<float> s_rumbleSmall{0.0f};

std::atomic<bool> s_close{false};
std::atomic<bool> s_visible{true};

std::mutex s_headMutex;
HeadPose s_headPose;
HeadTrackedFrame s_headFrame;
std::atomic<int64_t> s_builtKey{0};
PresentedFrame s_presented;
} // namespace

void publishSurface(const Surface &surface)
{
    {
        std::lock_guard<std::mutex> lock(s_surfaceMutex);
        if (s_surface.window)
            ANativeWindow_release(s_surface.window);
        s_surface = surface;
        if (s_surface.window)
            ANativeWindow_acquire(s_surface.window); // our own reference, independent of the XR thread's
    }
    s_surfaceChanged.notify_all();
}

bool waitForSurface(Surface *surface, int timeoutMs)
{
    std::unique_lock<std::mutex> lock(s_surfaceMutex);
    const bool ready = s_surfaceChanged.wait_for(lock, std::chrono::milliseconds(timeoutMs), [] {
        return s_surface.window != nullptr || s_close.load();
    });
    if (!ready || !s_surface.window)
        return false;
    if (surface)
        *surface = s_surface;
    return true;
}

void publishPad(const Pad &pad)
{
    std::lock_guard<std::mutex> lock(s_padMutex);
    s_pad = pad;
}

Pad currentPad()
{
    std::lock_guard<std::mutex> lock(s_padMutex);
    return s_pad;
}

void requestMenuToggle()
{
    std::lock_guard<std::mutex> lock(s_menuMutex);
    s_menuToggle = true;
}

bool takeMenuToggle()
{
    std::lock_guard<std::mutex> lock(s_menuMutex);
    return std::exchange(s_menuToggle, false);
}

void postMenuKey(uint32_t key, bool down)
{
    std::lock_guard<std::mutex> lock(s_menuMutex);
    if (s_menuKeys.size() < 64)
        s_menuKeys.emplace_back(key, down);
}

bool takeMenuKey(uint32_t *key, bool *down)
{
    std::lock_guard<std::mutex> lock(s_menuMutex);
    if (s_menuKeys.empty())
        return false;
    *key = s_menuKeys.front().first;
    *down = s_menuKeys.front().second;
    s_menuKeys.pop_front();
    return true;
}

bool menuOpen() { return s_menuOpen.load(std::memory_order_relaxed); }
void setMenuOpen(bool open) { s_menuOpen.store(open, std::memory_order_relaxed); }

static std::atomic<bool> s_curvedView{true}; // default: curved screen (owner choice)
void setCurvedView(bool curved)
{
    if (s_curvedView.exchange(curved, std::memory_order_relaxed) != curved)
        std::fprintf(stderr, "[quest] race view: %s\n", curved ? "curved screen" : "VR");
}
bool curvedView() { return s_curvedView.load(std::memory_order_relaxed); }

void setRumble(float large, float small)
{
    s_rumbleLarge.store(large, std::memory_order_relaxed);
    s_rumbleSmall.store(small, std::memory_order_relaxed);
}

void rumble(float *large, float *small)
{
    *large = s_rumbleLarge.load(std::memory_order_relaxed);
    *small = s_rumbleSmall.load(std::memory_order_relaxed);
}

void requestClose()
{
    s_close.store(true);
    s_surfaceChanged.notify_all();
}

bool closeRequested() { return s_close.load(); }

void setVisible(bool visible) { s_visible.store(visible, std::memory_order_relaxed); }
bool visible() { return s_visible.load(std::memory_order_relaxed); }

void publishHeadPose(const HeadPose &pose)
{
    std::lock_guard<std::mutex> lock(s_headMutex);
    s_headPose = pose;
}

HeadPose currentHeadPose()
{
    std::lock_guard<std::mutex> lock(s_headMutex);
    return s_headPose;
}

void noteHeadTrackedFrame(int64_t key, float tanHalfX, float tanHalfY, bool immersive)
{
    std::lock_guard<std::mutex> lock(s_headMutex);
    s_headFrame.immersive = immersive;
    s_headFrame.key = key;
    s_builtKey.store(key, std::memory_order_relaxed);
    s_headFrame.tanHalfX = tanHalfX;
    s_headFrame.tanHalfY = tanHalfY;
    ++s_headFrame.serial;
}

// GT4 shows a camera two vblanks after the one that follows its build (measured on the PC
// 2026-10-30: a test yaw built in frame 6400 first shows in the picture of vblank 6403; the tag of
// a vblank is the camera built in the frame before it). So the tag of this vblank is the key built
// kBuiltKeyDelay vblanks earlier. Live: debug.gt4vr.keydelay (0..7).
int64_t takeBuiltKey()
{
    constexpr int kBuiltKeyDelay = 2;
    static int64_t history[8] = {};
    static unsigned next = 0;
    static int delay = kBuiltKeyDelay;
    static int countdown = 0;
    if (countdown-- <= 0)
    {
        countdown = 60;
        char value[PROP_VALUE_MAX] = {};
        delay = __system_property_get("debug.gt4vr.keydelay", value) > 0 ? std::atoi(value) : kBuiltKeyDelay;
        delay = delay < 0 ? 0 : delay > 7 ? 7 : delay;
    }
    history[next & 7u] = s_builtKey.exchange(0, std::memory_order_relaxed);
    const int64_t key = history[(next - static_cast<unsigned>(delay)) & 7u];
    ++next;
    return key;
}

static std::atomic<int (*)()> s_vsyncPresented{nullptr};
void setVsyncPresentedQuery(int (*query)())
{
    s_vsyncPresented.store(query, std::memory_order_release);
}

void notePresentedKey(int64_t key)
{
    if (const auto query = s_vsyncPresented.load(std::memory_order_acquire); query && query() == 0)
        return; // skipped field (setVsyncPresentedQuery)
    if (key == 0)
        return; // no new head-tracked camera this field: the surface shows the previous picture
    std::lock_guard<std::mutex> lock(s_headMutex);
    s_presented.key = key;
    ++s_presented.serial;
}

PresentedFrame lastPresentedKey()
{
    std::lock_guard<std::mutex> lock(s_headMutex);
    return s_presented;
}

HeadTrackedFrame lastHeadTrackedFrame()
{
    std::lock_guard<std::mutex> lock(s_headMutex);
    return s_headFrame;
}
} // namespace gt4::quest

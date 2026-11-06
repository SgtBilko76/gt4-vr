// SPDX-License-Identifier: GPL-3.0-or-later
//
// The seam between the Quest OpenXR thread (quest_xr.cpp) and the Fukami product host
// (quest_presentation.cpp, quest_audio.cpp). Everything here is thread-safe: the XR thread
// writes, the game threads read, and nothing in this header knows OpenXR or PCSX2.
#pragma once

#include <cstdint>

struct ANativeWindow;

namespace gt4::quest
{
// The XR thread's render target: the ANativeWindow of an XR_KHR_android_surface_swapchain.
// The game hands it to the PCSX2 GS bridge, which renders into it with Vulkan.
struct Surface
{
    ANativeWindow *window = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
};

// XR thread: publish the surface (once the session is running) or withdraw it (nullptr).
void publishSurface(const Surface &surface);
// Game thread: wait up to timeoutMs for a surface; false on timeout or shutdown.
bool waitForSurface(Surface *surface, int timeoutMs);

// DualShock 2 button bits, in the order of the pad's button word (ps2::set_pad_buttons).
enum HostPadButton : uint32_t
{
    HostPadButtonSelect = 1u << 0, HostPadButtonL3 = 1u << 1, HostPadButtonR3 = 1u << 2, HostPadButtonStart = 1u << 3,
    HostPadButtonUp = 1u << 4, HostPadButtonRight = 1u << 5, HostPadButtonDown = 1u << 6, HostPadButtonLeft = 1u << 7,
    HostPadButtonL2 = 1u << 8, HostPadButtonR2 = 1u << 9, HostPadButtonL1 = 1u << 10, HostPadButtonR1 = 1u << 11,
    HostPadButtonTriangle = 1u << 12, HostPadButtonCircle = 1u << 13, HostPadButtonCross = 1u << 14, HostPadButtonSquare = 1u << 15,
};

// PS2 DualShock 2 state for port 0, written by the XR thread every frame.
struct Pad
{
    bool connected = false;
    uint32_t buttons = 0;               // HostPadButton bits
    float leftX = 0.0f, leftY = 0.0f;   // positive Y is up
    float rightX = 0.0f, rightY = 0.0f;
    float l2 = 0.0f, r2 = 0.0f;         // analog trigger pressure 0..1
    // GT4 reads the pressure of Cross (throttle) and Square (brake): 0..1 while held.
    float cross = 0.0f, square = 0.0f;
};
void publishPad(const Pad &pad);
Pad currentPad();

// Fukami's in-game menu (src/app/fukami_menu.h): the XR thread asks for a toggle and posts
// navigation keys (RRV_PCSX2_GS_UI_KEY_* values); the presentation drains them on the game's
// main thread.
void requestMenuToggle();
bool takeMenuToggle();
void postMenuKey(uint32_t key, bool down);
bool takeMenuKey(uint32_t *key, bool *down);
bool menuOpen();
void setMenuOpen(bool open);

// Race view (menu "Race view", display.vr_view; default curved): false = VR (head-tracked game camera filling
// the view), true = the race on the curved 3D screen (game camera, depth stereo).
void setCurvedView(bool curved);
bool curvedView();

// Rumble from the guest (0..1 per motor), applied as Touch haptics by the XR thread.
void setRumble(float large, float small);
void rumble(float *large, float *small);

// Shutdown: the activity is finishing or the XR session ended for good.
void requestClose();
bool closeRequested();

// Session visibility: the game keeps running while hidden, but audio is muted (the Quest
// otherwise plays the race on in the background while the user is in the system menu).
void setVisible(bool visible);
bool visible();

// Head tracking (Stage 2b). The XR thread publishes the head orientation relative to the
// screen's forward direction (an OpenXR quaternion, predicted for when a frame rendered now
// will be on screen) and its key time; the game's camera patch (quest_headtrack.cpp) turns the
// race camera by it and reports back the field of view of the projection it used and which
// key time it rendered with, so the XR thread can show that frame where it was looked at.
struct HeadPose
{
    float qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 1.0f;
    // Head position relative to where the view was centred (metres, same axes as the rotation).
    float px = 0.0f, py = 0.0f, pz = 0.0f;
    int64_t key = 0; // XR time the pose was predicted for (ns), 0 = none
    // Immersive race view: the field of view (tan of the half angles) the race camera is to
    // render with; 0 = keep the game's own projection (flat screen).
    float renderTanX = 0.0f, renderTanY = 0.0f;
    float renderShiftY = 0.0f; // picture window moved down (tan), see gt4::camera::Head::shiftY
};
void publishHeadPose(const HeadPose &pose);
HeadPose currentHeadPose();
struct HeadTrackedFrame
{
    int64_t key = 0;        // HeadPose::key the camera used
    float tanHalfX = 0.0f;  // the game projection's tan(horizontal half field of view)
    float tanHalfY = 0.0f;
    uint64_t serial = 0;    // increments with every head-tracked camera build
    bool immersive = false; // rendered with HeadPose::renderTanX/Y
};
void noteHeadTrackedFrame(int64_t key, float tanHalfX, float tanHalfY, bool immersive);
// Which head pose the picture on the XR surface was rendered with. The GS backend takes the key
// of the field's camera when the field's vsync is queued (takeBuiltKey, game side) and reports it
// once that vsync has presented (notePresentedKey, GS thread); the XR thread shows the surface at
// exactly that pose (lastPresentedKey). 0 = this field built no head-tracked camera.
int64_t takeBuiltKey();
void notePresentedKey(int64_t key);
// The bridge's rrv_pcsx2_gs_bridge_android_vsync_presented (set by the XR thread once it has the
// bridge loaded): a skipped field presented nothing, so its key is not the picture's pose.
void setVsyncPresentedQuery(int (*query)());
struct PresentedFrame
{
    int64_t key = 0;
    uint64_t serial = 0;
};
PresentedFrame lastPresentedKey();
HeadTrackedFrame lastHeadTrackedFrame();
} // namespace gt4::quest

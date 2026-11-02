// SPDX-License-Identifier: GPL-3.0-or-later
//
// Gran Turismo 4 on Meta Quest: the OpenXR thread (taken over from the Ridge Racer V port, Fukami).
//
// Stage 1 (this file): an immersive OpenXR session that shows the game on a big virtual screen.
// XR_KHR_android_surface_swapchain gives an ordinary Android Surface; its ANativeWindow is handed
// to the PCSX2 GS bridge (quest_host.h -> quest_presentation.cpp -> RRV_PCSX2_GS_SURFACE_ANDROID),
// which renders into it with Vulkan exactly like a desktop window. This thread only submits that
// swapchain as a world-locked layer and turns the Touch controllers into a DualShock 2.
// Derived from the user's ARMSX2-3D QuestXr.cpp (platforms/android/app/src/main/cpp/xr).
//
// Head-tracked stereo (stage 3) replaces the single screen with per-eye renders of the game's
// own camera; the surface is sized so both eye images fit side by side later.
#include "quest_xr.h"

#include "quest_host.h"
#include "rrv_pcsx2_gs_bridge.h"
#include "online_panel.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <sys/system_properties.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <dlfcn.h>

#define XR_LOG(...) __android_log_print(ANDROID_LOG_INFO, "GT4VR-XR", __VA_ARGS__)
#define XR_ERR(...) __android_log_print(ANDROID_LOG_ERROR, "GT4VR-XR", __VA_ARGS__)

namespace
{
// The game picture: 16:9 (Fukami's real widescreen, rrv.ini ratio = 16:9), one per eye. The
// surface is double-width: PCSX2's GSStereo (ARMSX2 fork, quest/bridge/android_stereo.cpp) draws
// the frame into each half, reprojected left/right by the depth buffer of its main 3D pass, and
// each eye's layer samples its own half.
// Each half is 1920 x 1440: the immersive race view is nearly square (the headset's field of view,
// ~112 x 114 degrees), and 1440 lines carry the 672 the GS renders at scale 3 with room to spare.
// The menu screen keeps its 16:9 shape (kScreenAspect) whatever the pixels are.
// Default eye size (used until the runtime's recommendation is known, and when it is unusable).
// InitScreen sizes the eyes like QuakeQuest (owner 2026-10-30: "schau bei QuakeQuest nach den besten
// Quest Settings"): the runtime's recommended eye resolution x a supersampling factor (QuakeQuest's
// SS_MULTIPLIER 1.3; config.txt RRV_VR_SS), scaled to the immersive view's field of view.
constexpr int32_t kEyeWidthDefaultPx = 1920;
constexpr int32_t kScreenHeightDefaultPx = 1440;
constexpr float kEyeSupersample = 1.3f;
constexpr float kHeadsetTan = 1.40f; // Quest 3 eye: tan 1.376 x 1.428 (108 x 110 deg)
constexpr float kScreenAspect = 16.0f / 9.0f;
// The HUD layer (owner 2026-10-30: "take our Wipeout Rewrite mod for perfect HUD VR settings"):
// below the eyes the surface has a 16:9 strip the GS draws the race HUD into, on transparent
// black (GSStereo::hud_layer). In the immersive race view it is a flat screen of its own in front
// of the 3D view, Wipeout Rewrite VR's: 3.2 m wide, 2.5 m ahead, fixed in the room.
constexpr int32_t kHudWidthPx = 1920;
constexpr int32_t kHudRowsPx = 1080;
constexpr float kHudDistanceM = 2.0f; // Wipeout Pure VR: PURE_VR_HUD_DISTANCE=2.0 (Wipeout-Rewrite-VR had 2.5)
constexpr float kHudWorldWidthM = 1.8f; // Wipeout Pure VR: PURE_VR_HUD_WIDTH=1.8 (Wipeout-Rewrite-VR had 3.2)

// Depth stereo strength (ARMSX2-3D defaults). separation = horizontal shift in source UV at
// depth 1; convergence = the depth that lands on the screen plane. Live tuning:
//   adb shell setprop debug.gt4vr.sep 0.015
//   adb shell setprop debug.gt4vr.conv 0.0
//   adb shell setprop debug.gt4vr.stereo 0       (flat: both eyes the same picture)
//   adb shell setprop debug.gt4vr.showdepth 1    (diagnostic: draw the depth buffer)
constexpr float kStereoSeparation = 0.011f;
constexpr float kStereoConvergence = 0.0f;
// Depth packing scale for the screen stereo. GT4's race depth packs to almost 0 unscaled; measured on
// the Nurburgring (2026-10-30, debug.gt4vr.showdepth 1): 4096 dark, 8192 road near ~0.6 and trees
// mid-grey, 16384 near road saturated, 65536 everything white. Live: debug.gt4vr.depthscale.
constexpr float kScreenDepthScale = 8192.0f;
using SetStereoFn = void (*)(int, float, float);

// Head tracking (Stage 2b, quest_headtrack.cpp turns the race camera). The game samples the
// head pose predicted for now + kHeadLagMs, and a frame it renders is on screen about that much
// later; in a head-tracked frame the screen is shown flat, where the head was predicted to be
// for it, at the size that matches the game's field of view, so the world stays put while the
// picture follows the view. Live tuning:
//   adb shell setprop debug.gt4vr.headtrack 0    (off: fixed screen, camera not turned)
//   adb shell setprop debug.gt4vr.headlag 50     (ms from sampling to display)
//   adb shell setprop debug.gt4vr.headdist 1.5   (metres to the head-tracked screen)
constexpr float kHeadLagMs = 50.0f;
constexpr float kHeadScreenDistanceM = 1.5f;

XrQuaternionf QuatMul(const XrQuaternionf &a, const XrQuaternionf &b)
{
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

XrQuaternionf QuatConj(const XrQuaternionf &q) { return {-q.x, -q.y, -q.z, q.w}; }

XrVector3f QuatRotate(const XrQuaternionf &q, const XrVector3f &v)
{
    const XrQuaternionf p = QuatMul(QuatMul(q, {v.x, v.y, v.z, 0.0f}), QuatConj(q));
    return {p.x, p.y, p.z};
}
using SetStereoOptionsFn = void (*)(int, float, int);
using SetHudVrFn = void (*)(float, float);
using SetDepthScaleFn = void (*)(float);
using SetPresentAspectFn = void (*)(int);

// True-scale stereo in the immersive race view (as Wipeout-Rewrite-VR renders each eye from its
// own pose): the frame is reprojected per eye by its depth so that an object at z metres gets the
// disparity the headset's eye distance gives it, IPD / 2 / z per eye; infinity has none. RR5's
// 24-bit Z is 2^24 / z (z in game units, near plane 1, far 65535; func_220B88), i.e. PCSX2's depth
// is 1 / (256 z). The packed depth (8 bits) is scaled so that 1 is kNearM and 0 is infinity;
// anything nearer than kNearM, the HUD included (it is drawn at the near plane), sits at kNearM.
// kUnitsPerMeter: game units per metre (debug.gt4vr.upm), measured from the race speed.
//   adb shell setprop debug.gt4vr.nearm 2.0   (HUD distance, metres)
//   adb shell setprop debug.gt4vr.upm 100
//   adb shell setprop debug.gt4vr.stereoscale 1.0   (1 = true scale)
constexpr float kNearM = 2.5f; // the HUD sits where Wipeout-Rewrite-VR's HUD screen is
// GT4 (gt4_camera.cpp): units are metres and the driving view's near plane is 0.1 (P = GL frustum,
// P22 -1.0001, P32 -0.2, measured on the Nurburgring 2026-10-30), so its Z is about 2^24 * 0.1 / z:
// for this formula (written for RR5's 2^24 / z) that is 10 "units" per metre. Cross-check: the screen
// stereo's depth x8192 showed the road ~5 m ahead at ~0.6 (2^24 * 0.1 / 5.3 / 2^32 * 8192 = 0.6).
constexpr float kUnitsPerMeter = 10.0f;

// Immersive race view (owner direction 2026-10-28, after Wipeout-Rewrite-VR): in a race the game
// renders with the headset's field of view (plus kFovMargin for head motion between render and
// display) and the frame is shown as a projection layer at the eye poses it was rendered for, so
// there is no screen: the compositor's timewarp keeps the world still. The race HUD is brought
// into the middle of the view (kHudHalfWidth = tan of its half width). Live tuning:
//   adb shell setprop debug.gt4vr.immersive 0    (race on the head-tracked screen instead)
//   adb shell setprop debug.gt4vr.fovmargin 1.1
//   adb shell setprop debug.gt4vr.hudw 0.6
constexpr float kFovMargin = 1.08f;
// Immersive race field of view (degrees, square), Wipeout Pure VR's race camera: 110. Live:
// adb shell setprop debug.gt4vr.fov 90   (0 = the headset's whole view)
// Owner choice 2026-10-30: 90 (sharper; no black border visible in the headset).
constexpr float kImmersiveFovDeg = 110.0f; // Owner choice 2026-10-30: 110 (Wipeout Rewrite VR: 65)
constexpr float kHudHalfWidth = 0.45f; // Wipeout Pure VR: 1.8 m wide at 2 m

// ~77 degrees wide at 1.5 m, like ARMSX2-3D. Live tuning:
//   adb shell setprop debug.gt4vr.screenw 3.0   (metres)
//   adb shell setprop debug.gt4vr.dist 2.0      (metres)
//   adb shell setprop debug.gt4vr.curved 0
// Owner 2026-10-30: the curved screen a bit closer and 110 degrees wide (was Wipeout-Rewrite-VR's
// 3.2 m at 2.5 m, 65 degrees). Live: debug.gt4vr.dist (m), debug.gt4vr.screenfov (deg);
// debug.gt4vr.screenw (m) still overrides the width.
// Owner 2026-11-01: 5.71 m at 2.0 m (110 deg) put the HUD outside the Quest's ~104 deg view; moved
// back to 3.0 m at the same width (~87 deg), "looks good".
constexpr float kScreenDistanceM = 3.0f;
constexpr float kScreenWidthM = 5.71f;
// Screen height off the head / eye level, in metres; negative = the screen sits below the eyes
// (owner: the screen was much too high -> correct the vertical height, put it below eye level).
// Live tuning:
//   adb shell setprop debug.gt4vr.screenu -0.8    (metres, negative = lower than the eyes)
constexpr float kScreenVerticalOffsetM = 0.0f;
// VR mode OFF (owner 2026-11-01, after the per-eye VR tests: "keep the curved screen as the main
// mode", then "turn off the switch"): both stick clicks no longer switch to the old head-tracked VR
// race view; the curved HD 3D screen is the only race view. debug.gt4vr.vrallow 1 re-enables the
// switch for tests, debug.gt4vr.vr 1 / 0 still selects VR / curved headless.
constexpr bool kVrModeAllowed = false;
std::atomic<bool> g_vr_allowed{kVrModeAllowed}; // live: debug.gt4vr.vrallow

float ReadFloatProperty(const char *name, float fallback)
{
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get(name, value) <= 0)
        return fallback;
    const float parsed = std::strtof(value, nullptr);
    return std::isfinite(parsed) ? parsed : fallback;
}

using InitializeLoaderFn = XrResult(XRAPI_PTR *)(const XrLoaderInitInfoBaseHeaderKHR *);
using GetGlesRequirementsFn = XrResult(XRAPI_PTR *)(XrInstance, XrSystemId, XrGraphicsRequirementsOpenGLESKHR *);
using CreateSurfaceSwapchainFn = XrResult(XRAPI_PTR *)(XrSession, const XrSwapchainCreateInfo *, XrSwapchain *, jobject *);
using EnumerateRefreshRatesFn = XrResult(XRAPI_PTR *)(XrSession, uint32_t, uint32_t *, float *);
using GetRefreshRateFn = XrResult(XRAPI_PTR *)(XrSession, float *);
using RequestRefreshRateFn = XrResult(XRAPI_PTR *)(XrSession, float);
using SetPerformanceLevelFn = XrResult(XRAPI_PTR *)(XrSession, XrPerfSettingsDomainEXT, XrPerfSettingsLevelEXT);

template <typename Fn>
bool LoadFunction(XrInstance instance, const char *name, Fn &out)
{
    PFN_xrVoidFunction function = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(instance, name, &function)) || !function)
        return false;
    out = reinterpret_cast<Fn>(function);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Touch -> DualShock 2, GT4 layout (as in the first GT4 Quest build, which the user raced with):
//   right trigger = Cross with pressure (throttle)      left trigger = Square with pressure (brake)
//   A = Cross (confirm)  B = Triangle (back)  X = Circle (handbrake)  Y = Select (change view)
//   right grip = R2 (shift up)  left grip = L2 (shift down)
//   left stick = left analog stick (steering, menu pointer)      right stick = d-pad
//   left / right stick click = L1 / R1                            Menu button = Start (pause)
//   both stick clicks together = switch the race view (curved screen <-> VR), once per press
//   (off while kVrModeAllowed is false).
// GT4 reads Cross/Square pressure, so the triggers give analog throttle and brake.
struct ControllerInput
{
    bool a = false, b = false, x = false, y = false, menu = false;
    bool leftStickClick = false, rightStickClick = false;
    float leftTrigger = 0, rightTrigger = 0, leftGrip = 0, rightGrip = 0;
    float leftX = 0, leftY = 0, rightX = 0, rightY = 0;
};

class TouchMapper
{
public:
    gt4::quest::Pad Update(const ControllerInput &in, double now)
    {
        (void)now;
        // A trigger counts as held above 8 % travel (released below 4 %), with its travel as pressure.
        const auto trigger = [](bool &held, float v) {
            held = held ? v > 0.04f : v > 0.08f;
            return held ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
        };
        const auto hyst = [](bool &state, float value) {
            if (!state && value > 0.45f)
                state = true;
            else if (state && value < 0.25f)
                state = false;
            return state;
        };
        const float throttle = trigger(m_rt, in.rightTrigger);
        const float brake = trigger(m_lt, in.leftTrigger);
        const bool lg = hyst(m_lg, in.leftGrip);
        const bool rg = hyst(m_rg, in.rightGrip);
        const auto stickDir = [](bool &state, float value, float sign) {
            const float v = value * sign;
            if (!state && v > 0.6f)
                state = true;
            else if (state && v < 0.4f)
                state = false;
            return state;
        };
        const bool up = stickDir(m_rs[0], in.rightY, 1.0f), down = stickDir(m_rs[1], in.rightY, -1.0f);
        const bool left = stickDir(m_rs[2], in.rightX, -1.0f), right = stickDir(m_rs[3], in.rightX, 1.0f);

        if (in.leftStickClick && in.rightStickClick && !m_stickCombo)
        {
            m_stickCombo = true;
            // GT4 Online: the online menu (owner 2026-11-02). The VR view switch only when VR mode is
            // allowed again (debug.gt4vr.vrallow); otherwise (Spec II release) the combo does nothing.
            if (g_vr_allowed.load(std::memory_order_relaxed))
                gt4::quest::setCurvedView(!gt4::quest::curvedView());
            else if (gt4::panel::available())
                gt4::panel::toggle();
        }
        else if (!in.leftStickClick && !in.rightStickClick)
        {
            m_stickCombo = false;
        }

        // Online menu open: the right stick picks the tab (left/right) and scrolls (up/down), B closes
        // it, and the game gets a released pad (pause GT4 first if a race is running).
        const bool panel_b = in.b && !m_panelB;
        m_panelB = in.b;
        const bool was_a = m_panelA;
        m_panelA = in.a;
        if (gt4::panel::isOpen())
        {
            const int dx = (right && !m_panelDir[3]) - (left && !m_panelDir[2]);
            const int dy = (down && !m_panelDir[1]) - (up && !m_panelDir[0]);
            if (dx || dy)
                gt4::panel::input(dx, dy);
            if (panel_b)
                gt4::panel::setOpen(false);
            if (in.a && !was_a)
                gt4::panel::select();
            m_panelDir[0] = up, m_panelDir[1] = down, m_panelDir[2] = left, m_panelDir[3] = right;
            m_panelHeld = true;
            gt4::quest::Pad idle;
            idle.connected = true;
            return idle;
        }
        // After closing: keep the pad released until B and the stick are let go, so the closing
        // press doesn't reach the game.
        if (m_panelHeld)
        {
            if (in.a || in.b || up || down || left || right)
            {
                gt4::quest::Pad idle;
                idle.connected = true;
                return idle;
            }
            m_panelHeld = false;
        }

        gt4::quest::Pad pad;
        pad.connected = true;
        uint32_t b = 0;
        if (in.a || throttle > 0.0f) b |= gt4::quest::HostPadButtonCross;
        if (brake > 0.0f) b |= gt4::quest::HostPadButtonSquare;
        if (in.b) b |= gt4::quest::HostPadButtonTriangle;
        if (in.x) b |= gt4::quest::HostPadButtonCircle;
        if (in.y) b |= gt4::quest::HostPadButtonSelect;
        if (rg) b |= gt4::quest::HostPadButtonR2;
        if (lg) b |= gt4::quest::HostPadButtonL2;
        if (in.leftStickClick && !m_stickCombo) b |= gt4::quest::HostPadButtonL1;
        if (in.rightStickClick && !m_stickCombo) b |= gt4::quest::HostPadButtonR1;
        if (in.menu) b |= gt4::quest::HostPadButtonStart;
        if (up) b |= gt4::quest::HostPadButtonUp;
        if (down) b |= gt4::quest::HostPadButtonDown;
        if (left) b |= gt4::quest::HostPadButtonLeft;
        if (right) b |= gt4::quest::HostPadButtonRight;
        pad.buttons = b;
        pad.leftX = in.leftX;
        pad.leftY = in.leftY;
        pad.cross = in.a ? 1.0f : throttle;
        pad.square = brake;
        return pad;
    }

    void Reset()
    {
        *this = TouchMapper();
    }

private:
    bool m_rt = false, m_lt = false, m_lg = false, m_rg = false;
    bool m_stickCombo = false;
    bool m_rs[4] = {};
    bool m_panelA = false, m_panelB = false, m_panelHeld = false;
    bool m_panelDir[4] = {};
};

struct HeadSample
{
    XrTime key = 0;
    XrTime published = 0; // display time of the XR frame that published it
    XrPosef pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
    XrPosef eyes[2] = {};
    bool have_eyes = false;
};

class QuestXr
{
public:
    QuestXr(JavaVM *vm, jobject activity) : m_vm(vm), m_activity(activity) {}

    void Run()
    {
        JNIEnv *env = nullptr;
        if (m_vm->AttachCurrentThread(&env, nullptr) != JNI_OK)
        {
            XR_ERR("AttachCurrentThread failed");
            gt4::quest::requestClose();
            return;
        }
        if (InitEgl() && InitInstance() && InitSession() && InitActions() && InitScreen(env))
        {
            while (!gt4::quest::closeRequested() || m_session_running)
            {
                PollEvents();
                if (m_exit_requested && !m_session_running)
                    break;
                if (!m_session_running)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    continue;
                }
                if (gt4::quest::closeRequested() && !m_exit_requested)
                {
                    m_exit_requested = true;
                    xrRequestExitSession(m_session);
                }
                RunFrame();
            }
        }
        else
        {
            XR_ERR("init failed: %s", m_error.c_str());
        }
        gt4::quest::requestClose();
        Destroy();
        m_vm->DetachCurrentThread();
    }

private:
    struct Actions
    {
        XrActionSet set = XR_NULL_HANDLE;
        XrAction a = XR_NULL_HANDLE, b = XR_NULL_HANDLE, x = XR_NULL_HANDLE, y = XR_NULL_HANDLE;
        XrAction menu = XR_NULL_HANDLE;
        XrAction leftStickClick = XR_NULL_HANDLE, rightStickClick = XR_NULL_HANDLE;
        XrAction leftTrigger = XR_NULL_HANDLE, rightTrigger = XR_NULL_HANDLE;
        XrAction leftGrip = XR_NULL_HANDLE, rightGrip = XR_NULL_HANDLE;
        XrAction leftStick = XR_NULL_HANDLE, rightStick = XR_NULL_HANDLE;
        // Thumbstick axes also as float actions: the vector2f binding alone has read 0,0 on Quest.
        XrAction leftStickX = XR_NULL_HANDLE, leftStickY = XR_NULL_HANDLE;
        XrAction rightStickX = XR_NULL_HANDLE, rightStickY = XR_NULL_HANDLE;
        XrAction leftHaptic = XR_NULL_HANDLE, rightHaptic = XR_NULL_HANDLE;
    };

    bool Fail(const char *what, XrResult result = XR_SUCCESS)
    {
        m_error = what;
        if (result != XR_SUCCESS)
            m_error += " (XrResult " + std::to_string(static_cast<int>(result)) + ")";
        return false;
    }

    bool InitEgl()
    {
        m_egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (m_egl_display == EGL_NO_DISPLAY || !eglInitialize(m_egl_display, nullptr, nullptr))
            return Fail("eglInitialize failed");
        const EGLint config_attribs[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                         EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                         EGL_NONE};
        EGLint count = 0;
        if (!eglChooseConfig(m_egl_display, config_attribs, &m_egl_config, 1, &count) || count == 0)
            return Fail("no EGL config");
        const EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        m_egl_context = eglCreateContext(m_egl_display, m_egl_config, EGL_NO_CONTEXT, context_attribs);
        if (m_egl_context == EGL_NO_CONTEXT)
            return Fail("eglCreateContext failed");
        const EGLint pbuffer_attribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        m_egl_surface = eglCreatePbufferSurface(m_egl_display, m_egl_config, pbuffer_attribs);
        if (m_egl_surface == EGL_NO_SURFACE)
            return Fail("eglCreatePbufferSurface failed");
        if (!eglMakeCurrent(m_egl_display, m_egl_surface, m_egl_surface, m_egl_context))
            return Fail("eglMakeCurrent failed");
        return true;
    }

    bool InitInstance()
    {
        JNIEnv *env = nullptr;
        m_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
        XrLoaderInitInfoAndroidKHR loader_info{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
        loader_info.applicationVM = m_vm;
        loader_info.applicationContext = m_activity;
        InitializeLoaderFn initialize_loader = nullptr;
        if (!LoadFunction(XR_NULL_HANDLE, "xrInitializeLoaderKHR", initialize_loader))
            return Fail("OpenXR loader has no xrInitializeLoaderKHR");
        XrResult res = initialize_loader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR *>(&loader_info));
        if (XR_FAILED(res))
            return Fail("xrInitializeLoaderKHR failed", res);

        uint32_t ext_count = 0;
        res = xrEnumerateInstanceExtensionProperties(nullptr, 0, &ext_count, nullptr);
        if (XR_FAILED(res))
            return Fail("no OpenXR runtime", res);
        std::vector<XrExtensionProperties> props(ext_count, {XR_TYPE_EXTENSION_PROPERTIES});
        xrEnumerateInstanceExtensionProperties(nullptr, ext_count, &ext_count, props.data());
        const auto has_ext = [&props](const char *name) {
            for (const XrExtensionProperties &p : props)
                if (std::strcmp(p.extensionName, name) == 0)
                    return true;
            return false;
        };
        std::vector<const char *> extensions = {
            XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
            XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
            XR_KHR_ANDROID_SURFACE_SWAPCHAIN_EXTENSION_NAME,
        };
        for (const char *ext : extensions)
            if (!has_ext(ext))
                return Fail((std::string("runtime lacks ") + ext).c_str());
        if (has_ext(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME))
        {
            extensions.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
            m_has_refresh_rate_ext = true;
        }
        if (has_ext(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME))
        {
            extensions.push_back(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
            m_has_perf_settings_ext = true;
        }
        if (has_ext(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME))
        {
            extensions.push_back(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
            m_has_cylinder_ext = true;
        }
        if (has_ext(XR_FB_COMPOSITION_LAYER_IMAGE_LAYOUT_EXTENSION_NAME))
        {
            extensions.push_back(XR_FB_COMPOSITION_LAYER_IMAGE_LAYOUT_EXTENSION_NAME);
            m_has_image_layout_ext = true;
        }
        else
        {
            XR_ERR("%s unavailable: the picture will be upside down", XR_FB_COMPOSITION_LAYER_IMAGE_LAYOUT_EXTENSION_NAME);
        }

        XrInstanceCreateInfoAndroidKHR android_info{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
        android_info.applicationVM = m_vm;
        android_info.applicationActivity = m_activity;
        XrInstanceCreateInfo create_info{XR_TYPE_INSTANCE_CREATE_INFO};
        create_info.next = &android_info;
        std::strncpy(create_info.applicationInfo.applicationName, "Gran Turismo 4 VR", XR_MAX_APPLICATION_NAME_SIZE - 1);
        create_info.applicationInfo.applicationVersion = 1;
        std::strncpy(create_info.applicationInfo.engineName, "GT4VR", XR_MAX_ENGINE_NAME_SIZE - 1);
        create_info.applicationInfo.engineVersion = 1;
        create_info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
        create_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        create_info.enabledExtensionNames = extensions.data();
        res = xrCreateInstance(&create_info, &m_instance);
        if (XR_FAILED(res))
            return Fail("xrCreateInstance failed", res);

        XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
        system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        res = xrGetSystem(m_instance, &system_info, &m_system);
        if (XR_FAILED(res))
            return Fail("no head-mounted display", res);

        GetGlesRequirementsFn get_gles_requirements = nullptr;
        if (!LoadFunction(m_instance, "xrGetOpenGLESGraphicsRequirementsKHR", get_gles_requirements) ||
            !LoadFunction(m_instance, "xrCreateSwapchainAndroidSurfaceKHR", m_create_surface_swapchain))
            return Fail("runtime is missing required extension functions");
        if (m_has_perf_settings_ext)
            m_has_perf_settings_ext = LoadFunction(m_instance, "xrPerfSettingsSetPerformanceLevelEXT", m_set_performance_level);
        if (m_has_refresh_rate_ext)
            m_has_refresh_rate_ext = LoadFunction(m_instance, "xrEnumerateDisplayRefreshRatesFB", m_enumerate_refresh_rates) &&
                                     LoadFunction(m_instance, "xrGetDisplayRefreshRateFB", m_get_refresh_rate) &&
                                     LoadFunction(m_instance, "xrRequestDisplayRefreshRateFB", m_request_refresh_rate);
        XrGraphicsRequirementsOpenGLESKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
        res = get_gles_requirements(m_instance, m_system, &requirements);
        if (XR_FAILED(res))
            return Fail("xrGetOpenGLESGraphicsRequirementsKHR failed", res);
        return true;
    }

    bool InitSession()
    {
        XrGraphicsBindingOpenGLESAndroidKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
        binding.display = m_egl_display;
        binding.config = m_egl_config;
        binding.context = m_egl_context;
        XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
        session_info.next = &binding;
        session_info.systemId = m_system;
        XrResult res = xrCreateSession(m_instance, &session_info, &m_session);
        if (XR_FAILED(res))
            return Fail("xrCreateSession failed", res);
        XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        space_info.poseInReferenceSpace.orientation.w = 1.0f;
        space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        res = xrCreateReferenceSpace(m_session, &space_info, &m_local_space);
        if (XR_FAILED(res))
            return Fail("LOCAL space failed", res);
        space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        res = xrCreateReferenceSpace(m_session, &space_info, &m_view_space);
        if (XR_FAILED(res))
            return Fail("VIEW space failed", res);
        return true;
    }

    bool InitActions()
    {
        XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
        std::strncpy(set_info.actionSetName, "gameplay", XR_MAX_ACTION_SET_NAME_SIZE - 1);
        std::strncpy(set_info.localizedActionSetName, "Gameplay", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
        XrResult res = xrCreateActionSet(m_instance, &set_info, &m_actions.set);
        if (XR_FAILED(res))
            return Fail("xrCreateActionSet failed", res);

        struct ActionDef
        {
            XrAction *action;
            const char *name;
            XrActionType type;
            const char *path;
        };
        const ActionDef defs[] = {
            {&m_actions.a, "button_a", XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/right/input/a/click"},
            {&m_actions.b, "button_b", XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/right/input/b/click"},
            {&m_actions.x, "button_x", XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/left/input/x/click"},
            {&m_actions.y, "button_y", XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/left/input/y/click"},
            {&m_actions.menu, "menu", XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/left/input/menu/click"},
            {&m_actions.leftStickClick, "left_stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/left/input/thumbstick/click"},
            {&m_actions.rightStickClick, "right_stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/right/input/thumbstick/click"},
            {&m_actions.leftTrigger, "left_trigger", XR_ACTION_TYPE_FLOAT_INPUT, "/user/hand/left/input/trigger/value"},
            {&m_actions.rightTrigger, "right_trigger", XR_ACTION_TYPE_FLOAT_INPUT, "/user/hand/right/input/trigger/value"},
            {&m_actions.leftGrip, "left_grip", XR_ACTION_TYPE_FLOAT_INPUT, "/user/hand/left/input/squeeze/value"},
            {&m_actions.rightGrip, "right_grip", XR_ACTION_TYPE_FLOAT_INPUT, "/user/hand/right/input/squeeze/value"},
            {&m_actions.leftStick, "left_stick", XR_ACTION_TYPE_VECTOR2F_INPUT, "/user/hand/left/input/thumbstick"},
            {&m_actions.rightStick, "right_stick", XR_ACTION_TYPE_VECTOR2F_INPUT, "/user/hand/right/input/thumbstick"},
            {&m_actions.leftStickX, "left_stick_x", XR_ACTION_TYPE_FLOAT_INPUT, "/user/hand/left/input/thumbstick/x"},
            {&m_actions.leftStickY, "left_stick_y", XR_ACTION_TYPE_FLOAT_INPUT, "/user/hand/left/input/thumbstick/y"},
            {&m_actions.rightStickX, "right_stick_x", XR_ACTION_TYPE_FLOAT_INPUT, "/user/hand/right/input/thumbstick/x"},
            {&m_actions.rightStickY, "right_stick_y", XR_ACTION_TYPE_FLOAT_INPUT, "/user/hand/right/input/thumbstick/y"},
            {&m_actions.leftHaptic, "left_haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, "/user/hand/left/output/haptic"},
            {&m_actions.rightHaptic, "right_haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, "/user/hand/right/output/haptic"},
        };
        std::vector<XrActionSuggestedBinding> bindings;
        for (const ActionDef &def : defs)
        {
            XrActionCreateInfo action_info{XR_TYPE_ACTION_CREATE_INFO};
            action_info.actionType = def.type;
            std::strncpy(action_info.actionName, def.name, XR_MAX_ACTION_NAME_SIZE - 1);
            std::strncpy(action_info.localizedActionName, def.name, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
            res = xrCreateAction(m_actions.set, &action_info, def.action);
            if (XR_FAILED(res))
                return Fail("xrCreateAction failed", res);
            XrPath path = XR_NULL_PATH;
            xrStringToPath(m_instance, def.path, &path);
            bindings.push_back({*def.action, path});
        }
        XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        xrStringToPath(m_instance, "/interaction_profiles/oculus/touch_controller", &suggested.interactionProfile);
        suggested.suggestedBindings = bindings.data();
        suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
        res = xrSuggestInteractionProfileBindings(m_instance, &suggested);
        if (XR_FAILED(res))
            return Fail("Touch controller bindings refused", res);
        XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        attach.countActionSets = 1;
        attach.actionSets = &m_actions.set;
        res = xrAttachSessionActionSets(m_session, &attach);
        if (XR_FAILED(res))
            return Fail("xrAttachSessionActionSets failed", res);
        return true;
    }

    bool InitScreen(JNIEnv *env)
    {
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        // format, sampleCount, faceCount, arraySize and mipCount MUST be 0 for a surface swapchain
        // (the producer owns the buffer format); 1s fail with XR_ERROR_VALIDATION_FAILURE on Quest.
        {
            // QuakeQuest: recommendedImageRect x SS. Our eye image covers the immersive view's field
            // of view (kImmersiveFovDeg), not the whole headset view, so it is scaled down to it.
            uint32_t count = 0;
            XrViewConfigurationView views[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
            if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews(m_instance, m_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                               2, &count, views)) && count == 2 &&
                views[0].recommendedImageRectWidth > 0)
            {
                float ss = kEyeSupersample;
                if (const char *v = std::getenv("RRV_VR_SS"))
                    ss = std::clamp(static_cast<float>(std::atof(v)), 0.5f, 2.0f);
                const float tan_view = std::tan(kImmersiveFovDeg * 0.5f * 0.0174533f);
                const float k = ss * std::min(1.0f, tan_view / kHeadsetTan);
                // Never below the defaults: they already hold the GS picture at scale 3 (1920x1344).
                m_eye_w = std::clamp(static_cast<int32_t>(views[0].recommendedImageRectWidth * k + 0.5f) & ~7, kEyeWidthDefaultPx, 3072);
                m_eye_h = std::clamp(static_cast<int32_t>(views[0].recommendedImageRectHeight * k + 0.5f) & ~7, kScreenHeightDefaultPx, 3072);
                XR_LOG("eye image %dx%d (recommended %ux%u x SS %.2f, at %.0f deg)", m_eye_w, m_eye_h,
                       views[0].recommendedImageRectWidth, views[0].recommendedImageRectHeight, ss, kImmersiveFovDeg);
            }
            if (const char *v = std::getenv("RRV_VR_EYE"))
            {
                int w = 0, h = 0;
                if (std::sscanf(v, "%dx%d", &w, &h) == 2 && w >= 512 && h >= 512 && w <= 4096 && h <= 4096)
                    m_eye_w = w & ~7, m_eye_h = h & ~7;
            }
        }
        info.width = SurfaceWidth();
        info.height = SurfaceHeight();
        jobject surface = nullptr;
        const XrResult res = m_create_surface_swapchain(m_session, &info, &m_screen_swapchain, &surface);
        if (XR_FAILED(res) || surface == nullptr)
            return Fail("xrCreateSwapchainAndroidSurfaceKHR failed", res);
        m_screen_window = ANativeWindow_fromSurface(env, surface);
        if (!m_screen_window)
            return Fail("ANativeWindow_fromSurface failed");
        return true;
    }

    void Destroy()
    {
        gt4::quest::publishPad({});
        gt4::quest::publishSurface({});
        StopHaptics();
        if (m_screen_window)
            ANativeWindow_release(m_screen_window);
        if (m_screen_swapchain != XR_NULL_HANDLE)
            xrDestroySwapchain(m_screen_swapchain);
        if (m_panel_swapchain != XR_NULL_HANDLE)
            xrDestroySwapchain(m_panel_swapchain);
        if (m_local_space != XR_NULL_HANDLE)
            xrDestroySpace(m_local_space);
        if (m_view_space != XR_NULL_HANDLE)
            xrDestroySpace(m_view_space);
        if (m_actions.set != XR_NULL_HANDLE)
            xrDestroyActionSet(m_actions.set);
        if (m_session != XR_NULL_HANDLE)
            xrDestroySession(m_session);
        if (m_instance != XR_NULL_HANDLE)
            xrDestroyInstance(m_instance);
        if (m_egl_display != EGL_NO_DISPLAY)
        {
            eglMakeCurrent(m_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (m_egl_surface != EGL_NO_SURFACE)
                eglDestroySurface(m_egl_display, m_egl_surface);
            if (m_egl_context != EGL_NO_CONTEXT)
                eglDestroyContext(m_egl_display, m_egl_context);
        }
    }

    void PollEvents()
    {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(m_instance, &event) == XR_SUCCESS)
        {
            switch (event.type)
            {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
                OnSessionStateChanged(reinterpret_cast<const XrEventDataSessionStateChanged &>(event).state);
                break;
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                m_exit_requested = true;
                gt4::quest::requestClose();
                break;
            case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
                m_screen_placed = false; // the user recentred with the Meta button
                break;
            default:
                break;
            }
            event.type = XR_TYPE_EVENT_DATA_BUFFER;
            event.next = nullptr;
        }
    }

    void OnSessionStateChanged(XrSessionState state)
    {
        XR_LOG("session state %d", static_cast<int>(state));
        switch (state)
        {
        case XR_SESSION_STATE_READY:
        {
            XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
            begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            const XrResult res = xrBeginSession(m_session, &begin);
            if (XR_FAILED(res))
            {
                XR_ERR("xrBeginSession failed: %d", static_cast<int>(res));
                m_exit_requested = true;
                gt4::quest::requestClose();
                break;
            }
            m_session_running = true;
            PickRefreshRate();
            RequestPerformanceBoost();
            // Only now: before the session runs nothing consumes the Surface, and a GS presenting
            // into a full buffer queue would stall the whole game.
            if (!m_surface_published)
            {
                gt4::quest::publishSurface({m_screen_window, static_cast<uint32_t>(SurfaceWidth()), static_cast<uint32_t>(SurfaceHeight())});
                m_surface_published = true;
                XR_LOG("surface %dx%d (two eyes, HUD strip %d rows) handed to the game", SurfaceWidth(), SurfaceHeight(), kHudRowsPx);
            }
            break;
        }
        case XR_SESSION_STATE_STOPPING:
            xrEndSession(m_session);
            m_session_running = false;
            break;
        case XR_SESSION_STATE_EXITING:
        case XR_SESSION_STATE_LOSS_PENDING:
            m_exit_requested = true;
            gt4::quest::requestClose();
            break;
        default:
            break;
        }
        const bool focused = state == XR_SESSION_STATE_FOCUSED;
        if (focused != m_focused)
        {
            m_focused = focused;
            if (!focused)
            {
                m_mapper.Reset();
                gt4::quest::publishPad({true});
                StopHaptics();
            }
        }
        if (state == XR_SESSION_STATE_VISIBLE || state == XR_SESSION_STATE_FOCUSED)
            gt4::quest::setVisible(true);
        else if (state == XR_SESSION_STATE_SYNCHRONIZED || state == XR_SESSION_STATE_STOPPING ||
                 state == XR_SESSION_STATE_IDLE)
            gt4::quest::setVisible(false);
    }

    void PickRefreshRate()
    {
        if (!m_has_refresh_rate_ext)
            return;
        // The game presents at 60 fps. 120 Hz would show every frame twice with no judder, but the
        // compositor's share of the GPU at 120 Hz takes the race below 60 fps (measured 2026-10-28,
        // immersive race at scale 1: 42-47 fps at 120 Hz, 60 at 72 Hz), so 72 is the default (the
        // closest to 60 without 90's 3:2 cadence); debug.gt4vr.hz picks another rate live.
        uint32_t count = 0;
        if (XR_SUCCEEDED(m_enumerate_refresh_rates(m_session, 0, &count, nullptr)) && count > 0)
        {
            std::vector<float> rates(count);
            m_enumerate_refresh_rates(m_session, count, &count, rates.data());
            float pick = 0.0f;
            // adb shell setprop debug.gt4vr.hz 72 (live): a fixed rate instead.
            const float wanted = ReadFloatProperty("debug.gt4vr.hz", 0.0f);
            for (float rate : rates)
                if (wanted > 0.0f && std::fabs(rate - wanted) < 1.0f)
                    pick = rate;
            // 72 Hz (owner 2026-10-30, after the VR measurements: 90 Hz cost ~25 more skipped fields
            // per 600, 120 Hz takes the race below 60 fps); 120 next, as Wipeout Pure VR.
            for (float rate : rates)
                if (pick == 0.0f && std::fabs(rate - 72.0f) < 1.0f)
                    pick = rate;
            for (float rate : rates)
                if (pick == 0.0f && std::fabs(rate - 120.0f) < 1.0f)
                    pick = rate;
            if (pick == 0.0f)
                pick = *std::max_element(rates.begin(), rates.end());
            m_request_refresh_rate(m_session, pick);
        }
        float current = 0.0f;
        if (XR_SUCCEEDED(m_get_refresh_rate(m_session, &current)))
            XR_LOG("display refresh rate %.1f Hz", current);
    }

    void RequestPerformanceBoost()
    {
        if (!m_has_perf_settings_ext)
            return;
        const XrResult cpu = m_set_performance_level(m_session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, XR_PERF_SETTINGS_LEVEL_BOOST_EXT);
        const XrResult gpu = m_set_performance_level(m_session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, XR_PERF_SETTINGS_LEVEL_BOOST_EXT);
        XR_LOG("performance level BOOST: cpu %s, gpu %s", XR_SUCCEEDED(cpu) ? "ok" : "refused", XR_SUCCEEDED(gpu) ? "ok" : "refused");
    }

    void PollTuning()
    {
        if (++m_tuning_countdown < 30)
            return;
        m_tuning_countdown = 0;
        // Headless testing: debug.gt4vr.vr 1 / 0 selects VR / curved screen like both stick clicks.
        const int vr_prop = static_cast<int>(ReadFloatProperty("debug.gt4vr.vr", -1.0f));
        if (vr_prop != m_vr_prop)
        {
            m_vr_prop = vr_prop;
            if (vr_prop == 0 || vr_prop == 1)
                gt4::quest::setCurvedView(vr_prop == 0);
        }
        // Headless testing: debug.gt4vr.panel 1 / 0 opens / closes the online menu, 2..6 picks tab 1..5.
        const int panel_prop = static_cast<int>(ReadFloatProperty("debug.gt4vr.panel", -1.0f));
        if (panel_prop != m_panel_prop)
        {
            m_panel_prop = panel_prop;
            if (panel_prop >= 0)
                gt4::panel::setOpen(panel_prop != 0);
            if (panel_prop >= 2)
                gt4::panel::setTab(panel_prop - 2);
        }
        const bool vr_allowed = ReadFloatProperty("debug.gt4vr.vrallow", kVrModeAllowed ? 1.0f : 0.0f) != 0.0f;
        g_vr_allowed.store(vr_allowed, std::memory_order_relaxed);
        if (!vr_allowed && vr_prop != 1 && !gt4::quest::curvedView())
            gt4::quest::setCurvedView(true);
        const bool curved = ReadFloatProperty("debug.gt4vr.curved", 0.0f) != 0.0f || gt4::quest::curvedView();
        const float dist = std::max(0.3f, ReadFloatProperty("debug.gt4vr.dist", kScreenDistanceM));
        // Width: the fixed kScreenWidthM, so moving the screen away keeps its size (owner: farther =
        // the whole HUD in view). debug.gt4vr.screenfov (deg) sizes it by angle instead (chord width;
        // the curved layer turns it back into exactly that angle); debug.gt4vr.screenw overrides both.
        const float screen_fov = ReadFloatProperty("debug.gt4vr.screenfov", 0.0f);
        const float sized = screen_fov > 0.0f
                                ? 2.0f * dist * std::tan(std::clamp(screen_fov, 20.0f, 170.0f) * 0.5f * 0.0174533f)
                                : kScreenWidthM;
        const float width = std::max(0.2f, ReadFloatProperty("debug.gt4vr.screenw", sized));
        const float screen_vertical_m = std::clamp(ReadFloatProperty("debug.gt4vr.screenu", kScreenVerticalOffsetM), -1.0f, 1.0f);
        if (screen_vertical_m != m_screen_vertical_offset_m)
        {
            m_screen_vertical_offset_m = screen_vertical_m;
            m_screen_placed = false; // re-place on the next frame
        }
        if (curved != m_curved || width != m_screen_width_m || dist != m_screen_distance_m)
        {
            m_curved = curved;
            m_screen_width_m = width;
            m_screen_distance_m = dist;
            m_screen_placed = false;
            XR_LOG("screen: curved=%d width=%.2fm distance=%.2fm", curved ? 1 : 0, width, dist);
        }
        ApplyStereo();
        m_head_lag_ms = std::clamp(ReadFloatProperty("debug.gt4vr.headlag", 0.0f), 0.0f, 300.0f);
        m_lag_auto = m_head_lag_ms == 0.0f; // 0 (default) = measured
        m_head_dist_m = std::max(0.3f, ReadFloatProperty("debug.gt4vr.headdist", kHeadScreenDistanceM));
        // Race view "curved screen" (menu): no immersive race, the camera stays the game's.
        m_immersive_enabled = ReadFloatProperty("debug.gt4vr.immersive", 1.0f) != 0.0f && !gt4::quest::curvedView();
        const float hz = ReadFloatProperty("debug.gt4vr.hz", 0.0f);
        if (hz != m_wanted_hz)
        {
            m_wanted_hz = hz;
            if (m_session_running)
                PickRefreshRate();
        }
        m_fov_margin = std::clamp(ReadFloatProperty("debug.gt4vr.fovmargin", kFovMargin), 1.0f, 1.6f);
        // Field of view of the immersive race in degrees (debug.gt4vr.fov, live): the game renders
        // one picture for the whole view, so a narrower view puts more of its pixels per degree
        // (sharper) and leaves a black edge. 0 = the headset's whole view.
        {
            const float fov_deg = ReadFloatProperty("debug.gt4vr.fov", kImmersiveFovDeg);
            const float cap = fov_deg > 30.0f ? std::tan(fov_deg * 0.5f * 0.0174533f) : 0.0f;
            m_vtan_cap = ReadFloatProperty("debug.gt4vr.vtan", cap);
            m_htan_cap = ReadFloatProperty("debug.gt4vr.htan", cap);
        }
        m_hud_half_width = std::clamp(ReadFloatProperty("debug.gt4vr.hudw", kHudHalfWidth), 0.2f, 2.0f);
        m_hud_panel = ReadFloatProperty("debug.gt4vr.hudpanel", 0.0f) != 0.0f;
        m_hud_layer_on = ReadFloatProperty("debug.gt4vr.hudlayer", 1.0f) != 0.0f;
        // Headless testing without the user: hold pad buttons (HostPadButton mask, decimal) and
        // add a yaw (degrees, positive = head turned left) to the head the game camera sees.
        const uint32_t hold = static_cast<uint32_t>(ReadFloatProperty("debug.gt4vr.padhold", 0.0f));
        const float fake_yaw = ReadFloatProperty("debug.gt4vr.fakeyaw", 0.0f);
        const float fake_pitch = ReadFloatProperty("debug.gt4vr.fakepitch", 0.0f);
        m_fake_roll_deg = ReadFloatProperty("debug.gt4vr.fakeroll", 0.0f);
        // Headless: debug.gt4vr.menu changes value -> toggle the Fukami menu once.
        const float menu_req = ReadFloatProperty("debug.gt4vr.menu", 0.0f);
        if (m_menu_req_init && menu_req != m_menu_req)
        {
            m_menu_req = menu_req;
            gt4::quest::requestMenuToggle();
            XR_LOG("debug: menu toggle");
        }
        if (!m_menu_req_init)
        {
            m_menu_req_init = true; // a value left over from a previous run is no request
            m_menu_req = menu_req;
        }
        if (hold != m_debug_buttons || fake_yaw != m_fake_yaw_deg || fake_pitch != m_fake_pitch_deg)
            XR_LOG("debug: padhold=0x%04x fakeyaw=%.1f fakepitch=%.1f", hold, fake_yaw, fake_pitch);
        m_debug_buttons = hold;
        m_fake_yaw_deg = fake_yaw;
        m_fake_pitch_deg = fake_pitch;
    }

    // Publish the head orientation predicted for when a frame the game starts now is shown,
    // relative to the screen's forward direction, and remember the absolute pose by its time.
    void UpdateHead(XrTime display_time)
    {
        if (!m_screen_placed)
            return;
        // Predict the head for when a picture rendered now will be shown: the measured latency
        // (see ShownSample) unless debug.gt4vr.headlag fixes it.
        const float lag_ms = m_lag_auto ? m_lag_est_ms : m_head_lag_ms;
        const XrTime key = display_time + static_cast<XrTime>(lag_ms * 1.0e6f);
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(xrLocateSpace(m_view_space, m_local_space, key, &location)) ||
            !(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
            return;
        if (!(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
            location.pose.position = m_screen_pose.position;
        HeadSample sample;
        sample.key = key;
        sample.published = display_time;
        sample.pose = location.pose;
        // The eyes at the same time: the projection layer shows the frame from there.
        XrViewLocateInfo view_info{XR_TYPE_VIEW_LOCATE_INFO};
        view_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        view_info.displayTime = key;
        view_info.space = m_local_space;
        XrViewState view_state{XR_TYPE_VIEW_STATE};
        XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        uint32_t view_count = 0;
        if (XR_SUCCEEDED(xrLocateViews(m_session, &view_info, &view_state, 2, &view_count, views)) && view_count == 2 &&
            (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
        {
            sample.have_eyes = true;
            float tx = 0.0f, ty = 0.0f;
            for (uint32_t eye = 0; eye < 2; ++eye)
            {
                sample.eyes[eye] = views[eye].pose;
                if (!(view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT))
                    sample.eyes[eye].position = location.pose.position;
                const XrFovf &f = views[eye].fov;
                tx = std::max({tx, std::tan(-f.angleLeft), std::tan(f.angleRight)});
                ty = std::max({ty, std::tan(f.angleUp), std::tan(-f.angleDown)});
            }
            const XrVector3f &l = views[0].pose.position, &r = views[1].pose.position;
            const float ipd = std::sqrt((l.x - r.x) * (l.x - r.x) + (l.y - r.y) * (l.y - r.y) + (l.z - r.z) * (l.z - r.z));
            if (ipd > 0.04f && ipd < 0.09f)
            {
                if (std::fabs(ipd - m_ipd_m) > 0.0005f)
                    XR_LOG("eye distance %.1f mm", ipd * 1000.0f);
                m_ipd_m = ipd;
            }
            if (m_view_tan_x == 0.0f)
                XR_LOG("headset field of view: tan %.3f x %.3f (%.1f x %.1f deg per eye, symmetric)", tx, ty,
                       2.0f * std::atan(tx) * 57.29578f, 2.0f * std::atan(ty) * 57.29578f);
            m_view_tan_x = tx;
            m_view_tan_y = ty;
        }
        m_head_history[m_head_next] = sample;
        m_head_next = (m_head_next + 1) % kHeadHistory;
        XrQuaternionf rel = QuatMul(QuatConj(m_screen_pose.orientation), location.pose.orientation);
        if (m_fake_yaw_deg != 0.0f || m_fake_pitch_deg != 0.0f || m_fake_roll_deg != 0.0f)
        {
            // Positive: yaw left, pitch up, roll = head tilted to the left.
            const float yaw = m_fake_yaw_deg * 0.0174533f * 0.5f, pitch = m_fake_pitch_deg * 0.0174533f * 0.5f;
            const float roll = m_fake_roll_deg * 0.0174533f * 0.5f;
            rel = QuatMul(QuatMul(QuatMul({0.0f, std::sin(yaw), 0.0f, std::cos(yaw)}, {std::sin(pitch), 0.0f, 0.0f, std::cos(pitch)}),
                                  {0.0f, 0.0f, std::sin(roll), std::cos(roll)}), rel);
        }
        if (++m_head_log_countdown >= 180)
        {
            m_head_log_countdown = 0;
            const float yaw = std::atan2(2.0f * (rel.w * rel.y + rel.x * rel.z), 1.0f - 2.0f * (rel.y * rel.y + rel.x * rel.x));
            XR_LOG("head: yaw %.1f deg (positive = left) tracked=%d", yaw * 57.29578f, m_head_tracked ? 1 : 0);
        }
        gt4::quest::HeadPose head;
        {
            const XrVector3f d = {location.pose.position.x - m_screen_pose.position.x,
                                  location.pose.position.y - m_screen_pose.position.y,
                                  location.pose.position.z - m_screen_pose.position.z};
            const XrVector3f p = QuatRotate(QuatConj(m_screen_pose.orientation), d);
            head.px = p.x;
            head.py = p.y;
            head.pz = p.z;
        }
        head.qx = rel.x;
        head.qy = rel.y;
        head.qz = rel.z;
        head.qw = rel.w;
        head.key = key;
        // The Fukami menu is shown on the fixed screen (it would follow the head and fill the
        // view in the immersive race): while it is open the race camera is the game's own.
        if (gt4::quest::menuOpen())
            head.key = 0;
        if (head.key != 0 && m_immersive_enabled && m_view_tan_x > 0.0f)
        {
            head.renderTanX = m_view_tan_x * m_fov_margin;
            head.renderTanY = m_view_tan_y * m_fov_margin;
            // Optional visor: the game draws only 224 rows, so a smaller vertical field of view
            // puts more of them per degree (outside it the view is black). 0 = the full view.
            if (m_vtan_cap > 0.05f)
                head.renderTanY = std::min(head.renderTanY, m_vtan_cap);
            if (m_htan_cap > 0.05f)
                head.renderTanX = std::min(head.renderTanX, m_htan_cap);
        }
        gt4::quest::publishHeadPose(head);

        // Head-tracked while the game keeps turning cameras (menus and the intro are not).
        const gt4::quest::HeadTrackedFrame frame = gt4::quest::lastHeadTrackedFrame();
        if (frame.serial != m_head_serial)
        {
            m_head_serial = frame.serial;
            m_head_idle_frames = 0;
        }
        else if (m_head_idle_frames < 1000)
        {
            ++m_head_idle_frames;
        }
        const bool tracked = m_head_idle_frames < 20;
        if (tracked != m_head_tracked)
        {
            m_head_tracked = tracked;
            XR_LOG("screen: %s (game field of view tan %.3f x %.3f)", tracked ? "head-tracked race view" : "fixed",
                   frame.tanHalfX, frame.tanHalfY);
        }
    }

    // The pose the frame on screen now was rendered for: the published sample whose time is
    // closest to the display time.
    // The pose the picture on the surface was rendered with (reported by the GS backend when it
    // presented it), and the latency measurement from it. Falls back to the closest predicted
    // sample before the first report.
    const HeadSample *ShownSample(XrTime display_time)
    {
        const gt4::quest::PresentedFrame presented = gt4::quest::lastPresentedKey();
        const HeadSample *exact = nullptr;
        if (presented.key != 0)
            for (const HeadSample &s : m_head_history)
                if (s.key == presented.key)
                    exact = &s;
        if (!exact)
            return HeadSampleAt(display_time);
        if (presented.serial != m_presented_serial)
        {
            m_presented_serial = presented.serial;
            const float measured = static_cast<float>(display_time - exact->published) * 1.0e-6f;
            if (measured > 0.0f && measured < 400.0f)
                m_lag_est_ms += (measured - m_lag_est_ms) * 0.05f;
            if (++m_lag_log_countdown >= 600)
            {
                m_lag_log_countdown = 0;
                XR_LOG("head: picture latency %.1f ms (last %.1f)", m_lag_est_ms, measured);
            }
        }
        return exact;
    }

    const HeadSample *HeadSampleAt(XrTime display_time) const
    {
        const HeadSample *best = nullptr;
        for (const HeadSample &s : m_head_history)
        {
            if (s.key == 0)
                continue;
            if (!best || std::llabs(s.key - display_time) < std::llabs(best->key - display_time))
                best = &s;
        }
        return best;
    }

    bool HeadSampleFor(XrTime display_time, XrPosef *pose) const
    {
        const HeadSample *best = nullptr;
        for (const HeadSample &s : m_head_history)
        {
            if (s.key == 0)
                continue;
            if (!best || std::llabs(s.key - display_time) < std::llabs(best->key - display_time))
                best = &s;
        }
        if (!best)
            return false;
        *pose = best->pose;
        return true;
    }

    // The race HUD box (bridge.cpp rrv_quest_hud): its half width is m_hud_half_width (tan), its
    // 4:3 proportions are kept at the frame's aspect.
    void UpdateHudVr(bool immersive, float tan_x, float tan_y)
    {
        if (!m_set_hud_vr)
            return;
        // The HUD layer takes the HUD out of the immersive picture; the bridge then leaves it alone.
        const bool layer = immersive && m_hud_layer_on && m_set_hud_layer;
        if (m_set_hud_layer && layer != m_hud_layer_applied)
        {
            m_hud_layer_applied = layer;
            m_set_hud_layer(layer ? 1 : 0, kHudRowsPx);
            XR_LOG("race HUD layer: %s", layer ? "on (own screen, 3.2 m at 2.5 m)" : "off");
        }
        float scale = 0.0f, x = 1.0f;
        if (immersive && !layer)
        {
            // GT4's 2D layer (HUD, pause menu) is a 16:9 picture (anamorphic 640x448): the bridge
            // shrinks it into a panel m_hud_half_width (tan) wide, like Wipeout Pure VR's HUD
            // rectangle (1.8 m at 2 m). scale = y factor, x = extra x factor.
            // Default: the game's own HUD layout (owner 2026-10-30: the shrunk panel looked bad);
            // scale 1 still gives the HUD one stereo depth. debug.gt4vr.hudpanel 1 = the panel.
            if (m_hud_panel)
            {
                const float half_h = m_hud_half_width * 9.0f / 16.0f;
                scale = std::min(1.0f, half_h / tan_y);
                x = std::min(1.0f, m_hud_half_width / tan_x) / std::max(scale, 1e-3f);
            }
            else
            {
                scale = 1.0f;
                x = 1.0f;
            }
        }
        if (std::fabs(scale - m_hud_applied_scale) < 1e-4f && std::fabs(x - m_hud_applied_x) < 1e-4f)
            return;
        m_hud_applied_scale = scale;
        m_hud_applied_x = x;
        m_set_hud_vr(scale, x);
        XR_LOG("race HUD: %s (scale %.3f, x %.3f)", scale > 0.0f ? "middle box" : "game layout", scale, x);
    }

    // Immersive race: true-scale disparity (see kNearM); otherwise the screen settings.
    void UpdateTrueStereo(bool immersive, float tan_x)
    {
        if (!m_set_stereo || !m_set_depth_scale)
            return;
        const bool on = immersive && m_stereo_on && tan_x > 0.05f;
        if (on == m_true_stereo && (!on || std::fabs(tan_x - m_true_tan_x) < 1e-3f))
            return;
        m_true_stereo = on;
        m_true_tan_x = tan_x;
        if (on)
        {
            const float near_units = m_near_m * m_units_per_m;
            const float depth_scale = 256.0f * near_units;                    // packed 1 = near
            const float separation = m_stereo_scale * (m_ipd_m * 0.5f) / (m_near_m * 2.0f * tan_x); // UV per eye at 1
            m_set_depth_scale(depth_scale);
            m_set_stereo_options(1, m_stereo_show_depth, 0);
            m_set_stereo(1, separation, 0.0f);
            XR_LOG("stereo: true scale (eyes %.1f mm, near %.2f m = %.0f units, %.0f units/m, scale %.2f): "
                   "depth x%.0f, separation %.4f", m_ipd_m * 1000.0f, m_near_m, near_units, m_units_per_m,
                   m_stereo_scale, depth_scale, separation);
        }
        else
        {
            m_screen_depth_scale = -1.0f; // ApplyStereo puts the screen depth scale back
            m_stereo_applied = false; // ApplyStereo puts the screen settings back
            ApplyStereo();
        }
    }

    // The bridge is dlopen()ed by the GS backend once the game starts; until then there is
    // nothing to configure (and the GS has not presented yet either).
    void ApplyStereo()
    {
        if (m_stereo_unavailable)
            return;
        if (!m_set_stereo)
        {
            void *image = dlopen("librrv-pcsx2-gs-bridge.so", RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
            if (!image)
                return;
            m_set_stereo = reinterpret_cast<SetStereoFn>(dlsym(image, "rrv_pcsx2_gs_bridge_android_set_stereo"));
            m_set_stereo_options = reinterpret_cast<SetStereoOptionsFn>(dlsym(image, "rrv_pcsx2_gs_bridge_android_set_stereo_options"));
            m_set_hud_vr = reinterpret_cast<SetHudVrFn>(dlsym(image, "rrv_pcsx2_gs_bridge_android_set_hud_vr"));
            m_set_hud_layer = reinterpret_cast<SetHudLayerFn>(dlsym(image, "rrv_pcsx2_gs_bridge_android_set_hud_layer"));
            if (m_set_hud_layer)
                m_set_hud_layer(0, kHudRowsPx); // the eyes stay above the strip from the first frame on
            else
                XR_LOG("bridge has no HUD layer: the eyes fill the whole surface");
            m_set_depth_scale = reinterpret_cast<SetDepthScaleFn>(dlsym(image, "rrv_pcsx2_gs_bridge_android_set_stereo_depth_scale"));
            m_set_present_aspect = reinterpret_cast<SetPresentAspectFn>(dlsym(image, "rrv_pcsx2_gs_bridge_android_set_present_aspect"));
            gt4::quest::setVsyncPresentedQuery(
                reinterpret_cast<int (*)()>(dlsym(image, "rrv_pcsx2_gs_bridge_android_vsync_presented")));
            dlclose(image); // the GS backend keeps its own reference
            if (!m_set_stereo || !m_set_stereo_options)
            {
                XR_ERR("bridge has no stereo support: the picture spans both eyes");
                m_set_stereo = nullptr;
                m_stereo_unavailable = true;
                return;
            }
            m_stereo_applied = false;
        }
        const bool stereo = ReadFloatProperty("debug.gt4vr.stereo", 1.0f) != 0.0f;
        const float sep = stereo ? ReadFloatProperty("debug.gt4vr.sep", kStereoSeparation) : 0.0f;
        const float conv = ReadFloatProperty("debug.gt4vr.conv", kStereoConvergence);
        const float show_depth = ReadFloatProperty("debug.gt4vr.showdepth", 0.0f);
        const float near_m = std::clamp(ReadFloatProperty("debug.gt4vr.nearm", kNearM), 0.2f, 20.0f);
        const float upm = std::clamp(ReadFloatProperty("debug.gt4vr.upm", kUnitsPerMeter), 1.0f, 10000.0f);
        const float sscale = std::clamp(ReadFloatProperty("debug.gt4vr.stereoscale", 1.0f), 0.0f, 5.0f);
        if (near_m != m_near_m || upm != m_units_per_m || sscale != m_stereo_scale)
        {
            m_near_m = near_m;
            m_units_per_m = upm;
            m_stereo_scale = sscale;
            m_true_tan_x = -1.0f; // re-apply
        }
        if (m_true_stereo)
        {
            m_stereo_on = stereo;
            if (show_depth != m_stereo_show_depth)
            {
                m_stereo_show_depth = show_depth;
                m_set_stereo_options(1, show_depth, 0);
            }
            return; // UpdateTrueStereo owns the settings in the immersive race
        }
        // Depth packing scale on the screen (GSStereo::depth_scale): the packed depth is 8 bits of
        // clamp(depth * scale); a 24-bit Z reaches only 1/256 of PCSX2's depth range, so without a
        // scale the whole scene packs to 0 and the picture stays flat. Live: debug.gt4vr.depthscale.
        const float depth_scale = std::max(0.0f, ReadFloatProperty("debug.gt4vr.depthscale", kScreenDepthScale));
        if (m_set_depth_scale && depth_scale != m_screen_depth_scale)
        {
            m_screen_depth_scale = depth_scale;
            m_set_depth_scale(depth_scale);
            XR_LOG("stereo: screen depth scale %.1f", depth_scale);
        }
        if (m_stereo_applied && stereo == m_stereo_on && sep == m_stereo_sep && conv == m_stereo_conv &&
            show_depth == m_stereo_show_depth)
            return;
        m_stereo_applied = true;
        m_stereo_on = stereo;
        m_stereo_sep = sep;
        m_stereo_conv = conv;
        m_stereo_show_depth = show_depth;
        // Always split into eyes (the surface is double-width); "off" only skips the reprojection.
        m_set_stereo_options(stereo ? 1 : 0, show_depth, 0);
        m_set_stereo(1, sep, conv);
        XR_LOG("stereo: %s separation=%.4f convergence=%.3f showdepth=%.1f", stereo ? "depth" : "flat", sep, conv, show_depth);
    }

    void RunFrame()
    {
        XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState frame_state{XR_TYPE_FRAME_STATE};
        if (XR_FAILED(xrWaitFrame(m_session, &wait_info, &frame_state)))
            return;
        XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
        if (XR_FAILED(xrBeginFrame(m_session, &begin_info)))
            return;
        if (m_focused)
        {
            UpdateInput();
            UpdateHaptics();
        }
        PollTuning();
        if (frame_state.shouldRender)
            UpdateHead(frame_state.predictedDisplayTime);

        XrCompositionLayerQuad quads[2] = {{XR_TYPE_COMPOSITION_LAYER_QUAD}, {XR_TYPE_COMPOSITION_LAYER_QUAD}};
        XrCompositionLayerCylinderKHR cylinders[2] = {{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR},
                                                     {XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR}};
        // The compositor samples a surface swapchain GL-style (origin bottom-left); the GS writes
        // top row first like any Android window, so flip it in the compositor.
        XrCompositionLayerImageLayoutFB image_layout{XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB};
        image_layout.flags = XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB;
        const XrCompositionLayerBaseHeader *layers[4] = {};
        XrCompositionLayerQuad panel_quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        XrCompositionLayerQuad hud_quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        XrCompositionLayerProjectionView proj_views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                          {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        uint32_t layer_count = 0;
        if (frame_state.shouldRender)
        {
            if (!m_screen_placed)
                PlaceScreen(frame_state.predictedDisplayTime);
            XrPosef tracked_pose{};
            const gt4::quest::HeadTrackedFrame fov = gt4::quest::lastHeadTrackedFrame();
            const HeadSample *shown = m_head_tracked ? ShownSample(frame_state.predictedDisplayTime) : nullptr;
            const bool immersive = m_screen_placed && shown && shown->have_eyes && fov.immersive && fov.tanHalfX > 0.05f &&
                                   fov.tanHalfY > 0.05f && m_immersive_enabled;
            UpdateHudVr(immersive, fov.tanHalfX, fov.tanHalfY);
            UpdateTrueStereo(immersive, fov.tanHalfX);
            // Each eye's half is 4:3; the screen shows it as is, so the GS letterboxes the 16:9
            // game into it there and fills it (stretch) only in the immersive view.
            if (m_set_present_aspect && (immersive ? 1 : 0) != m_present_aspect)
            {
                m_present_aspect = immersive ? 1 : 0;
                m_set_present_aspect(m_present_aspect);
            }
            if (immersive != m_immersive_shown)
            {
                m_immersive_shown = immersive;
                XR_LOG("view: %s", immersive ? "immersive race (projection layer)" : "screen");
            }
            if (immersive)
            {
                // The frame as a projection layer: each eye's half from the eye pose it was
                // rendered for, at the game's (symmetric) field of view.
                const float ax = std::atan(fov.tanHalfX), ay = std::atan(fov.tanHalfY);
                for (uint32_t eye = 0; eye < 2; ++eye)
                {
                    XrCompositionLayerProjectionView &view = proj_views[eye];
                    view.pose = shown->eyes[eye];
                    view.fov = {-ax, ax, ay, -ay};
                    view.subImage.swapchain = m_screen_swapchain;
                    view.subImage.imageRect.offset = {static_cast<int32_t>(eye) * m_eye_w, 0};
                    view.subImage.imageRect.extent = {m_eye_w, m_eye_h};
                    view.subImage.imageArrayIndex = 0;
                }
                if (m_has_image_layout_ext)
                    projection.next = &image_layout;
                projection.space = m_local_space;
                projection.viewCount = 2;
                projection.views = proj_views;
                layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&projection);
                if (m_hud_layer_applied)
                {
                    // The HUD strip as its own screen in front (Wipeout Rewrite VR), alpha-blended.
                    if (m_has_image_layout_ext)
                        hud_quad.next = &image_layout;
                    hud_quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                    hud_quad.space = m_local_space;
                    hud_quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    hud_quad.subImage.swapchain = m_screen_swapchain;
                    hud_quad.subImage.imageRect.offset = {0, m_eye_h};
                    hud_quad.subImage.imageRect.extent = {kHudWidthPx, kHudRowsPx};
                    hud_quad.subImage.imageArrayIndex = 0;
                    hud_quad.pose = m_hud_pose;
                    hud_quad.size = {kHudWorldWidthM, kHudWorldWidthM * kHudRowsPx / kHudWidthPx};
                    layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&hud_quad);
                }
            }
            else if (m_screen_placed && m_head_tracked && !gt4::quest::menuOpen() && fov.tanHalfX > 0.05f &&
                     HeadSampleFor(frame_state.predictedDisplayTime, &tracked_pose))
            {
                // Head-tracked race view: a flat screen (the game's projection is planar) where
                // the head was for this frame, as wide as the game's field of view.
                const float width = 2.0f * m_head_dist_m * fov.tanHalfX;
                XrPosef pose = tracked_pose;
                const XrVector3f ahead = QuatRotate(pose.orientation, {0.0f, 0.0f, -m_head_dist_m});
                pose.position = {pose.position.x + ahead.x, pose.position.y + ahead.y, pose.position.z + ahead.z};
                for (uint32_t eye = 0; eye < 2; ++eye)
                {
                    XrCompositionLayerQuad &quad = quads[eye];
                    if (m_has_image_layout_ext)
                        quad.next = &image_layout;
                    quad.space = m_local_space;
                    quad.eyeVisibility = eye == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT;
                    quad.subImage.swapchain = m_screen_swapchain;
                    quad.subImage.imageRect.offset = {static_cast<int32_t>(eye) * m_eye_w, 0};
                    quad.subImage.imageRect.extent = {m_eye_w, m_eye_h};
                    quad.pose = pose;
                    quad.size = {width, width * fov.tanHalfY / fov.tanHalfX};
                    layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&quad);
                }
            }
            else if (m_screen_placed)
            {
                // The same screen in the same place for both eyes; only the half of the surface
                // each samples differs, and that difference is the stereo.
                for (uint32_t eye = 0; eye < 2; ++eye)
                {
                    XrSwapchainSubImage sub_image{};
                    sub_image.swapchain = m_screen_swapchain;
                    sub_image.imageRect.offset = {static_cast<int32_t>(eye) * m_eye_w, 0};
                    sub_image.imageRect.extent = {m_eye_w, m_eye_h};
                    const XrEyeVisibility visibility = eye == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT;
                    if (m_curved && m_has_cylinder_ext)
                    {
                        XrCompositionLayerCylinderKHR &cylinder = cylinders[eye];
                        if (m_has_image_layout_ext)
                            cylinder.next = &image_layout;
                        cylinder.space = m_local_space;
                        cylinder.eyeVisibility = visibility;
                        cylinder.subImage = sub_image;
                        cylinder.pose = m_screen_pose;
                        cylinder.radius = m_screen_distance_m;
                        cylinder.centralAngle = 2.0f * std::atan((m_screen_width_m * 0.5f) / m_screen_distance_m);
                        cylinder.aspectRatio = static_cast<float>(m_eye_w) / static_cast<float>(m_eye_h);
                        layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&cylinder);
                    }
                    else
                    {
                        XrCompositionLayerQuad &quad = quads[eye];
                        if (m_has_image_layout_ext)
                            quad.next = &image_layout;
                        quad.space = m_local_space;
                        quad.eyeVisibility = visibility;
                        quad.subImage = sub_image;
                        quad.pose = m_flat_screen_pose;
                        quad.size = {m_screen_width_m, m_screen_width_m * m_eye_h / m_eye_w};
                        layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&quad);
                    }
                }
            }
        }
        // GT4 Online's online menu: its own quad in front of everything, where the head looked when
        // it was opened.
        if (frame_state.shouldRender && UpdatePanel(frame_state.predictedDisplayTime))
        {
            panel_quad.space = m_local_space;
            panel_quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            panel_quad.subImage.swapchain = m_panel_swapchain;
            panel_quad.subImage.imageRect.extent = {gt4::panel::kWidth, gt4::panel::kHeight};
            panel_quad.pose = m_panel_pose;
            panel_quad.size = {kPanelWidthM, kPanelWidthM * gt4::panel::kHeight / gt4::panel::kWidth};
            layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&panel_quad);
        }
        XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
        end_info.displayTime = frame_state.predictedDisplayTime;
        end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        end_info.layerCount = layer_count;
        end_info.layers = layer_count ? layers : nullptr;
        xrEndFrame(m_session, &end_info);
    }

    // ---- online menu (online_panel.h)
    static constexpr float kPanelWidthM = 1.1f;     // 1.1 m wide ...
    static constexpr float kPanelDistanceM = 1.25f; // ... at 1.25 m: about 47 degrees
    static constexpr float kPanelBelowEyeM = 0.08f;

    // Returns true when the panel layer is to be shown this frame.
    bool UpdatePanel(XrTime time)
    {
        const bool open = gt4::panel::isOpen();
        if (!open)
        {
            m_panel_shown = false;
            return false;
        }
        if (m_panel_swapchain == XR_NULL_HANDLE && !m_panel_failed && !CreatePanelSwapchain())
        {
            m_panel_failed = true;
            gt4::panel::setOpen(false);
            return false;
        }
        if (m_panel_failed)
            return false;
        if (!m_panel_shown)
        {
            // Opened: in front of the head, upright, turned towards it (yaw only).
            XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
            if (XR_FAILED(xrLocateSpace(m_view_space, m_local_space, time, &location)) ||
                !(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
                return false;
            const XrQuaternionf &q = location.pose.orientation;
            const float fx = -2.0f * (q.x * q.z + q.w * q.y);
            const float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
            const float yaw = std::atan2(-fx, -fz);
            const XrVector3f &head = location.pose.position;
            m_panel_pose.orientation = {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
            m_panel_pose.position = {head.x - std::sin(yaw) * kPanelDistanceM, head.y - kPanelBelowEyeM,
                                     head.z - std::cos(yaw) * kPanelDistanceM};
            m_panel_shown = true;
        }
        if (gt4::panel::render(m_panel_pixels))
        {
            // The compositor samples GL-style (first row = bottom): upload the rows bottom-up.
            const size_t row = static_cast<size_t>(gt4::panel::kWidth) * 4;
            m_panel_flipped.resize(m_panel_pixels.size());
            for (int y = 0; y < gt4::panel::kHeight; ++y)
                std::memcpy(&m_panel_flipped[(gt4::panel::kHeight - 1 - y) * row], &m_panel_pixels[y * row], row);
            uint32_t index = 0;
            XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wait.timeout = XR_INFINITE_DURATION;
            if (XR_SUCCEEDED(xrAcquireSwapchainImage(m_panel_swapchain, &acquire, &index)) &&
                XR_SUCCEEDED(xrWaitSwapchainImage(m_panel_swapchain, &wait)))
            {
                glBindTexture(GL_TEXTURE_2D, m_panel_images[index].image);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, gt4::panel::kWidth, gt4::panel::kHeight, GL_RGBA, GL_UNSIGNED_BYTE,
                                m_panel_flipped.data());
                glBindTexture(GL_TEXTURE_2D, 0);
                glFlush();
                XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                xrReleaseSwapchainImage(m_panel_swapchain, &release);
                m_panel_uploaded = true;
            }
        }
        return m_panel_uploaded;
    }

    bool CreatePanelSwapchain()
    {
        uint32_t count = 0;
        xrEnumerateSwapchainFormats(m_session, 0, &count, nullptr);
        std::vector<int64_t> formats(count);
        xrEnumerateSwapchainFormats(m_session, count, &count, formats.data());
        // sRGB: the panel's colours are sRGB values and the compositor shows them as such.
        int64_t format = 0;
        for (int64_t want : {static_cast<int64_t>(GL_SRGB8_ALPHA8), static_cast<int64_t>(GL_RGBA8)})
            if (!format && std::find(formats.begin(), formats.end(), want) != formats.end())
                format = want;
        if (!format)
        {
            XR_ERR("online menu: no RGBA8 swapchain format");
            return false;
        }
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        info.format = format;
        info.sampleCount = 1;
        info.width = gt4::panel::kWidth;
        info.height = gt4::panel::kHeight;
        info.faceCount = 1;
        info.arraySize = 1;
        info.mipCount = 1;
        const XrResult res = xrCreateSwapchain(m_session, &info, &m_panel_swapchain);
        if (XR_FAILED(res))
        {
            XR_ERR("online menu: xrCreateSwapchain failed (%d)", static_cast<int>(res));
            m_panel_swapchain = XR_NULL_HANDLE;
            return false;
        }
        uint32_t images = 0;
        xrEnumerateSwapchainImages(m_panel_swapchain, 0, &images, nullptr);
        m_panel_images.assign(images, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
        xrEnumerateSwapchainImages(m_panel_swapchain, images, &images,
                                   reinterpret_cast<XrSwapchainImageBaseHeader *>(m_panel_images.data()));
        XR_LOG("online menu: swapchain %dx%d, format 0x%llx, %u images", gt4::panel::kWidth, gt4::panel::kHeight,
               static_cast<unsigned long long>(format), images);
        return true;
    }

    // Straight ahead of the head at eye height, upright (yaw only).
    void PlaceScreen(XrTime time)
    {
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(xrLocateSpace(m_view_space, m_local_space, time, &location)))
            return;
        const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if ((location.locationFlags & needed) != needed)
            return;
        const XrQuaternionf &q = location.pose.orientation;
        const float fx = -2.0f * (q.x * q.z + q.w * q.y);
        const float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
        const float yaw = std::atan2(-fx, -fz);
        const XrVector3f &head = location.pose.position;
        const XrQuaternionf yaw_orientation{0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
        const float screen_y = head.y + m_screen_vertical_offset_m;
        m_screen_pose.orientation = yaw_orientation;
        m_screen_pose.position = {head.x, screen_y, head.z};
        m_flat_screen_pose.orientation = yaw_orientation;
        m_flat_screen_pose.position = {head.x - std::sin(yaw) * m_screen_distance_m, screen_y,
                                       head.z - std::cos(yaw) * m_screen_distance_m};
        m_hud_pose.orientation = yaw_orientation;
        m_hud_pose.position = {head.x - std::sin(yaw) * kHudDistanceM, screen_y, head.z - std::cos(yaw) * kHudDistanceM};
        m_screen_placed = true;
    }

    bool GetBool(XrAction action)
    {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
        return XR_SUCCEEDED(xrGetActionStateBoolean(m_session, &info, &state)) && state.isActive && state.currentState;
    }

    float GetFloat(XrAction action)
    {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = action;
        XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
        return (XR_SUCCEEDED(xrGetActionStateFloat(m_session, &info, &state)) && state.isActive) ? state.currentState : 0.0f;
    }

    XrVector2f GetStick(XrAction vector, XrAction x, XrAction y)
    {
        XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
        info.action = vector;
        XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
        XrVector2f v{0.0f, 0.0f};
        if (XR_SUCCEEDED(xrGetActionStateVector2f(m_session, &info, &state)) && state.isActive)
            v = state.currentState;
        if (v.x == 0.0f && v.y == 0.0f)
            v = {GetFloat(x), GetFloat(y)};
        return v;
    }

    void UpdateInput()
    {
        XrActiveActionSet active{m_actions.set, XR_NULL_PATH};
        XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
        sync.countActiveActionSets = 1;
        sync.activeActionSets = &active;
        if (XR_FAILED(xrSyncActions(m_session, &sync)))
            return;
        ControllerInput in;
        in.a = GetBool(m_actions.a);
        in.b = GetBool(m_actions.b);
        in.x = GetBool(m_actions.x);
        in.y = GetBool(m_actions.y);
        in.menu = GetBool(m_actions.menu);
        in.leftStickClick = GetBool(m_actions.leftStickClick);
        in.rightStickClick = GetBool(m_actions.rightStickClick);
        in.leftTrigger = GetFloat(m_actions.leftTrigger);
        in.rightTrigger = GetFloat(m_actions.rightTrigger);
        in.leftGrip = GetFloat(m_actions.leftGrip);
        in.rightGrip = GetFloat(m_actions.rightGrip);
        const XrVector2f left = GetStick(m_actions.leftStick, m_actions.leftStickX, m_actions.leftStickY);
        const XrVector2f right = GetStick(m_actions.rightStick, m_actions.rightStickX, m_actions.rightStickY);
        in.leftX = left.x;
        in.leftY = left.y;
        in.rightX = right.x;
        in.rightY = right.y;
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        gt4::quest::Pad pad = m_mapper.Update(in, now);
        pad.buttons |= m_debug_buttons; // headless testing: debug.gt4vr.padhold
        gt4::quest::publishPad(pad);
        // Two-second input log while anything is pressed: proves the Touch -> pad chain on-head.
        if ((pad.buttons != 0u || std::fabs(pad.leftX) > 0.2f) && now - m_lastInputLog > 2.0)
        {
            m_lastInputLog = now;
            XR_LOG("input: buttons=0x%04x lx=%.2f ly=%.2f l2=%.2f r2=%.2f menu=%d", pad.buttons, pad.leftX, pad.leftY,
                   pad.l2, pad.r2, gt4::quest::menuOpen() ? 1 : 0);
        }
    }

    // PS2 rumble -> Touch haptics: large motor leans left, small motor right (ARMSX2-3D).
    void UpdateHaptics()
    {
        float large = 0.0f, small = 0.0f;
        gt4::quest::rumble(&large, &small);
        const float amplitudes[2] = {std::max(large, 0.5f * small), std::max(small, 0.5f * large)};
        const XrAction actions[2] = {m_actions.leftHaptic, m_actions.rightHaptic};
        const auto now = std::chrono::steady_clock::now();
        for (int hand = 0; hand < 2; hand++)
        {
            const float amplitude = std::clamp(amplitudes[hand], 0.0f, 1.0f);
            XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
            info.action = actions[hand];
            if (amplitude < 0.02f)
            {
                if (m_haptic_amplitude[hand] > 0.0f)
                    xrStopHapticFeedback(m_session, &info);
                m_haptic_amplitude[hand] = 0.0f;
                continue;
            }
            const bool changed = std::fabs(amplitude - m_haptic_amplitude[hand]) >= 0.05f;
            const bool expiring = (now - m_haptic_applied_at[hand]) >= std::chrono::milliseconds(200);
            if (!changed && !expiring)
                continue;
            XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
            vibration.duration = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::milliseconds(300)).count();
            vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
            vibration.amplitude = amplitude;
            xrApplyHapticFeedback(m_session, &info, reinterpret_cast<const XrHapticBaseHeader *>(&vibration));
            m_haptic_amplitude[hand] = amplitude;
            m_haptic_applied_at[hand] = now;
        }
    }

    void StopHaptics()
    {
        if (m_session == XR_NULL_HANDLE)
            return;
        const XrAction actions[2] = {m_actions.leftHaptic, m_actions.rightHaptic};
        for (int hand = 0; hand < 2; hand++)
        {
            if (m_haptic_amplitude[hand] <= 0.0f || actions[hand] == XR_NULL_HANDLE)
                continue;
            XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
            info.action = actions[hand];
            xrStopHapticFeedback(m_session, &info);
            m_haptic_amplitude[hand] = 0.0f;
        }
    }

    JavaVM *m_vm;
    jobject m_activity;
    std::string m_error;

    EGLDisplay m_egl_display = EGL_NO_DISPLAY;
    EGLConfig m_egl_config = nullptr;
    EGLContext m_egl_context = EGL_NO_CONTEXT;
    EGLSurface m_egl_surface = EGL_NO_SURFACE;

    XrInstance m_instance = XR_NULL_HANDLE;
    XrSystemId m_system = XR_NULL_SYSTEM_ID;
    XrSession m_session = XR_NULL_HANDLE;
    XrSpace m_local_space = XR_NULL_HANDLE;
    XrSpace m_view_space = XR_NULL_HANDLE;
    Actions m_actions;
    bool m_has_refresh_rate_ext = false;
    bool m_has_perf_settings_ext = false;
    bool m_has_cylinder_ext = false;
    bool m_has_image_layout_ext = false;
    SetPerformanceLevelFn m_set_performance_level = nullptr;
    CreateSurfaceSwapchainFn m_create_surface_swapchain = nullptr;
    EnumerateRefreshRatesFn m_enumerate_refresh_rates = nullptr;
    GetRefreshRateFn m_get_refresh_rate = nullptr;
    RequestRefreshRateFn m_request_refresh_rate = nullptr;

    bool m_session_running = false;
    bool m_focused = false;
    bool m_exit_requested = false;
    bool m_surface_published = false;
    float m_view_tan_x = 0.0f, m_view_tan_y = 0.0f;
    float m_ipd_m = 0.063f;
    SetDepthScaleFn m_set_depth_scale = nullptr;
    SetPresentAspectFn m_set_present_aspect = nullptr;
    int m_present_aspect = -1;
    bool m_true_stereo = false;
    float m_near_m = kNearM, m_units_per_m = kUnitsPerMeter, m_stereo_scale = 1.0f;
    float m_true_tan_x = 0.0f;
    uint64_t m_presented_serial = 0;
    float m_lag_est_ms = kHeadLagMs; // measured: publish -> first shown on the display
    int m_lag_log_countdown = 0;
    bool m_lag_auto = true;
    bool m_immersive_enabled = true;
    float m_wanted_hz = 0.0f;
    float m_fov_margin = kFovMargin;
    float m_vtan_cap = 0.0f, m_htan_cap = 0.0f;
    float m_hud_half_width = kHudHalfWidth;
    SetHudVrFn m_set_hud_vr = nullptr;
    float m_hud_applied_scale = -1.0f, m_hud_applied_x = -1.0f;
    bool m_immersive_shown = false;
    static constexpr size_t kHeadHistory = 64;
    HeadSample m_head_history[kHeadHistory];
    size_t m_head_next = 0;
    uint64_t m_head_serial = 0;
    int m_head_idle_frames = 1000;
    bool m_head_tracked = false;
    int m_head_log_countdown = 0;
    uint32_t m_debug_buttons = 0;
    XrSwapchain m_panel_swapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageOpenGLESKHR> m_panel_images;
    std::vector<uint8_t> m_panel_pixels, m_panel_flipped;
    XrPosef m_panel_pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.25f}};
    bool m_panel_shown = false, m_panel_uploaded = false, m_panel_failed = false;
    int m_panel_prop = -1;
    float m_fake_yaw_deg = 0.0f, m_fake_pitch_deg = 0.0f, m_fake_roll_deg = 0.0f;
    float m_menu_req = 0.0f;
    bool m_menu_req_init = false;
    float m_head_lag_ms = kHeadLagMs;
    float m_head_dist_m = kHeadScreenDistanceM;
    SetStereoFn m_set_stereo = nullptr;
    SetStereoOptionsFn m_set_stereo_options = nullptr;
    bool m_stereo_unavailable = false, m_stereo_applied = false, m_stereo_on = true;
    float m_stereo_sep = kStereoSeparation, m_stereo_conv = kStereoConvergence, m_stereo_show_depth = 0.0f;

    XrSwapchain m_screen_swapchain = XR_NULL_HANDLE;
    ANativeWindow *m_screen_window = nullptr;
    XrPosef m_screen_pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
    XrPosef m_flat_screen_pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -kScreenDistanceM}};
    bool m_screen_placed = false;
    bool m_curved = false;
    float m_screen_depth_scale = -1.0f;
    int m_vr_prop = -1;
    bool m_hud_panel = false;
    int32_t m_eye_w = kEyeWidthDefaultPx, m_eye_h = kScreenHeightDefaultPx;
    int32_t SurfaceWidth() const { return std::max(2 * m_eye_w, kHudWidthPx); }
    int32_t SurfaceHeight() const { return m_eye_h + kHudRowsPx; }
    bool m_hud_layer_on = true;
    bool m_hud_layer_applied = false;
    using SetHudLayerFn = void (*)(int, int);
    SetHudLayerFn m_set_hud_layer = nullptr;
    XrPosef m_hud_pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -kHudDistanceM}};
    float m_screen_width_m = 2.0f * kScreenDistanceM * 1.4281f; // tan(55 deg): 110 degrees
    float m_screen_distance_m = kScreenDistanceM;
    float m_screen_vertical_offset_m = kScreenVerticalOffsetM;
    int m_tuning_countdown = 0;

    float m_haptic_amplitude[2] = {0.0f, 0.0f};
    std::chrono::steady_clock::time_point m_haptic_applied_at[2] = {};
    TouchMapper m_mapper;
    double m_lastInputLog = 0.0;
};
} // namespace

namespace gt4::quest
{
void runXrThread(JavaVM *vm, jobject activity)
{
    QuestXr xr(vm, activity);
    xr.Run();
}
} // namespace gt4::quest
